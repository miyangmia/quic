// SPDX-License-Identifier: GPL-2.0-or-later
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Initialization/cleanup for QUIC protocol support.
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include "socket.h"
#include "frame.h"

static void quic_inq_rfree(struct sk_buff *skb)
{
	atomic_sub(skb->len, &skb->sk->sk_rmem_alloc);
}

void quic_inq_set_owner_r(struct sk_buff *skb, struct sock *sk)
{
	skb_orphan(skb);
	skb->sk = sk;
	atomic_add(skb->len, &sk->sk_rmem_alloc);
	skb->destructor = quic_inq_rfree;
}

static int quic_new_sock_do_rcv(struct sock *sk, struct sk_buff *skb,
				union quic_addr *da, union quic_addr *sa)
{
	struct sock *nsk;
	int ret = 0;

	local_bh_disable();
	nsk = quic_sock_lookup(skb, da, sa);
	if (nsk == sk)
		goto out;
	/* the request sock was just accepted */
	bh_lock_sock(nsk);
	if (sock_owned_by_user(nsk)) {
		if (sk_add_backlog(nsk, skb, READ_ONCE(nsk->sk_rcvbuf)))
			kfree_skb(skb);
	} else {
		sk->sk_backlog_rcv(nsk, skb);
	}
	bh_unlock_sock(nsk);
	ret = 1;
out:
	local_bh_enable();
	return ret;
}

int quic_handshake_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	union quic_addr da, sa;

	if (!quic_is_listen(sk))
		goto out;

	quic_af_ops(sk)->get_msg_addr(&da, skb, 0);
	quic_af_ops(sk)->get_msg_addr(&sa, skb, 1);
	if (quic_request_sock_exists(sk, &da, &sa))
		goto out;

	if (QUIC_RCV_CB(skb)->backlog &&
	    quic_new_sock_do_rcv(sk, skb, &da, &sa))
		return 0;

	if (!quic_hdr(skb)->form) {
		kfree_skb(skb);
		return -EINVAL;
	}

	if (quic_request_sock_enqueue(sk, &da, &sa)) {
		kfree_skb(skb);
		return -ENOMEM;
	}
out:
	if (atomic_read(&sk->sk_rmem_alloc) + skb->len > sk->sk_rcvbuf) {
		kfree_skb(skb);
		return -ENOBUFS;
	}

	quic_inq_set_owner_r(skb, sk);
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	sk->sk_data_ready(sk);
	return 0;
}

int quic_do_rcv(struct sock *sk, struct sk_buff *skb)
{
	if (!quic_is_connected(sk)) {
		kfree_skb(skb);
		return 0;
	}

	return quic_packet_process(sk, skb);
}

int quic_rcv(struct sk_buff *skb)
{
	struct quic_source_connection_id *s_conn_id;
	struct quic_addr_family_ops *af_ops;
	union quic_addr daddr, saddr;
	struct sock *sk = NULL;
	int err = -EINVAL;
	u8 *dcid;

	skb_pull(skb, skb_transport_offset(skb));
	af_ops = quic_af_ops_get(ip_hdr(skb)->version == 4 ? AF_INET : AF_INET6);

	if (skb->len < sizeof(struct quichdr))
		goto err;

	if (!quic_hdr(skb)->form) {
		dcid = (u8 *)quic_hdr(skb) + 1;
		s_conn_id = quic_source_connection_id_lookup(dev_net(skb->dev),
							     dcid, skb->len - 1);
		if (s_conn_id) {
			QUIC_RCV_CB(skb)->number_offset =
				s_conn_id->common.id.len + sizeof(struct quichdr);
			sk = s_conn_id->sk;
		}
	}
	if (!sk) {
		af_ops->get_msg_addr(&daddr, skb, 0);
		af_ops->get_msg_addr(&saddr, skb, 1);
		sk = quic_sock_lookup(skb, &daddr, &saddr);
		if (!sk)
			goto err;
	}
	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		QUIC_RCV_CB(skb)->backlog = 1;
		if (sk_add_backlog(sk, skb, READ_ONCE(sk->sk_rcvbuf))) {
			bh_unlock_sock(sk);
			goto err;
		}
	} else {
		sk->sk_backlog_rcv(sk, skb);
	}
	bh_unlock_sock(sk);
	return 0;

err:
	kfree_skb(skb);
	return err;
}

static void quic_inq_recv_tail(struct sock *sk, struct quic_stream *stream, struct sk_buff *skb)
{
	struct quic_stream_update update = {};

	if (QUIC_RCV_CB(skb)->stream_fin) {
		update.id = stream->id;
		update.state = QUIC_STREAM_RECV_STATE_RECVD;
		update.errcode = QUIC_RCV_CB(skb)->stream_offset + skb->len;
		quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update);
		stream->recv.state = update.state;
	}
	stream->recv.offset += skb->len;
	quic_inq_set_owner_r(skb, sk);
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	sk->sk_data_ready(sk);
}

int quic_inq_flow_control(struct sock *sk, struct quic_stream *stream, int len)
{
	struct quic_inqueue *inq = quic_inq(sk);
	struct sk_buff *nskb = NULL;

	if (!len)
		return 0;

	stream->recv.bytes += len;
	inq->bytes += len;

	/* recv flow control */
	if (inq->max_bytes - inq->bytes < inq->window / 2) {
		inq->max_bytes = inq->bytes + inq->window;
		nskb = quic_frame_create(sk, QUIC_FRAME_MAX_DATA, inq);
		if (nskb)
			quic_outq_ctrl_tail(sk, nskb, true);
	}

	if (stream->recv.max_bytes - stream->recv.bytes < stream->recv.window / 2) {
		stream->recv.max_bytes = stream->recv.bytes + stream->recv.window;
		nskb = quic_frame_create(sk, QUIC_FRAME_MAX_STREAM_DATA, stream);
		if (nskb)
			quic_outq_ctrl_tail(sk, nskb, true);
	}

	if (!nskb)
		return 0;

	quic_outq_flush(sk);
	return 1;
}

int quic_inq_reasm_tail(struct sock *sk, struct sk_buff *skb)
{
	u64 stream_offset = QUIC_RCV_CB(skb)->stream_offset, offset;
	u64 stream_id = QUIC_RCV_CB(skb)->stream->id, highest = 0;
	struct quic_inqueue *inq = quic_inq(sk);
	struct quic_stream_update update = {};
	struct quic_stream *stream;
	struct sk_buff_head *head;
	struct sk_buff *tmp;

	stream = QUIC_RCV_CB(skb)->stream;
	if (stream->recv.offset > stream_offset) {
		kfree_skb(skb);
		return 0;
	}

	if (atomic_read(&sk->sk_rmem_alloc) + skb->len > sk->sk_rcvbuf)
		return -ENOBUFS;

	offset = stream_offset + skb->len;
	if (offset > stream->recv.highest) {
		highest = offset - stream->recv.highest;
		if (inq->highest + highest > inq->max_bytes ||
		    stream->recv.highest + highest > stream->recv.max_bytes)
			return -ENOBUFS;
	}
	if (!stream->recv.highest && !QUIC_RCV_CB(skb)->stream_fin) {
		update.id = stream->id;
		update.state = QUIC_STREAM_RECV_STATE_RECV;
		if (quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update))
			return -ENOMEM;
	}
	head = &inq->reassemble_list;
	if (stream->recv.offset < stream_offset) {
		skb_queue_walk(head, tmp) {
			if (QUIC_RCV_CB(tmp)->stream->id < stream_id)
				continue;
			if (QUIC_RCV_CB(tmp)->stream->id > stream_id)
				break;
			if (QUIC_RCV_CB(tmp)->stream_offset > stream_offset)
				break;
			if (QUIC_RCV_CB(tmp)->stream_offset == stream_offset) { /* dup */
				kfree_skb(skb);
				return 0;
			}
		}
		if (QUIC_RCV_CB(skb)->stream_fin) {
			update.id = stream->id;
			update.state = QUIC_STREAM_RECV_STATE_SIZE_KNOWN;
			update.errcode = QUIC_RCV_CB(skb)->stream_offset + skb->len;
			if (quic_inq_event_recv(sk, QUIC_EVENT_STREAM_UPDATE, &update))
				return -ENOMEM;
			stream->recv.state = update.state;
		}
		__skb_queue_before(head, tmp, skb);
		stream->recv.frags++;
		inq->highest += highest;
		stream->recv.highest += highest;
		return 0;
	}

	/* fast path: stream->recv.offset == stream_offset */
	inq->highest += highest;
	stream->recv.highest += highest;
	quic_inq_recv_tail(sk, stream, skb);
	if (!stream->recv.frags)
		return 0;

	skb_queue_walk_safe(head, skb, tmp) {
		if (QUIC_RCV_CB(skb)->stream->id < stream_id)
			continue;
		if (QUIC_RCV_CB(skb)->stream->id > stream_id)
			break;
		if (QUIC_RCV_CB(skb)->stream_offset > stream->recv.offset)
			break;
		__skb_unlink(skb, head);
		stream->recv.frags--;
		quic_inq_recv_tail(sk, stream, skb);
	}
	return 0;
}

void quic_inq_set_param(struct sock *sk, struct quic_transport_param *p)
{
	struct quic_inqueue *inq = quic_inq(sk);

	inq->max_udp_payload_size = p->max_udp_payload_size;
	inq->max_ack_delay = p->max_ack_delay;
	inq->ack_delay_exponent = p->ack_delay_exponent;
	inq->max_idle_timeout = p->max_idle_timeout;
	inq->window = p->initial_max_data;

	inq->max_bytes = p->initial_max_data;
	if (sk->sk_rcvbuf < p->initial_max_data * 2)
		sk->sk_rcvbuf = p->initial_max_data * 2;
}

void quic_inq_get_param(struct sock *sk, struct quic_transport_param *p)
{
	struct quic_inqueue *inq = quic_inq(sk);

	p->initial_max_data = inq->window;
	p->max_ack_delay = inq->max_ack_delay;
	p->ack_delay_exponent = inq->ack_delay_exponent;
	p->max_idle_timeout = inq->max_idle_timeout;
	p->max_udp_payload_size = inq->max_udp_payload_size;
}

int quic_inq_event_recv(struct sock *sk, u8 event, void *args)
{
	struct quic_stream *stream = NULL;
	struct sk_buff *skb, *last;
	int args_len = 0;

	if (!event || event > QUIC_EVENT_MAX)
		return -EINVAL;

	if (!(quic_inq(sk)->events & (1 << event)))
		return 0;

	switch (event) {
	case QUIC_EVENT_STREAM_UPDATE:
		stream = quic_stream_find(quic_streams(sk),
					  ((struct quic_stream_update *)args)->id);
		if (!stream)
			return -EINVAL;
		args_len = sizeof(struct quic_stream_update);
		break;
	case QUIC_EVENT_STREAM_MAX_STREAM:
		args_len = sizeof(u64);
		break;
	case QUIC_EVENT_NEW_TOKEN:
	case QUIC_EVENT_NEW_SESSION_TICKET:
		args_len = ((struct quic_token *)args)->len;
		args = ((struct quic_token *)args)->data;
		break;
	case QUIC_EVENT_CONNECTION_CLOSE:
		args_len = strlen(((struct quic_connection_close *)args)->phrase) +
			   1 + sizeof(struct quic_connection_close);
		break;
	case QUIC_EVENT_KEY_UPDATE:
		args_len = sizeof(u8);
		break;
	case QUIC_EVENT_CONNECTION_MIGRATION:
		args_len = sizeof(u8);
		break;
	default:
		return -EINVAL;
	}

	skb = alloc_skb(1 + args_len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, &event, 1);
	skb_put_data(skb, args, args_len);

	QUIC_RCV_CB(skb)->event = event;
	QUIC_RCV_CB(skb)->stream = stream;

	/* always put event ahead of data */
	last = quic_inq(sk)->last_event ?: (struct sk_buff *)&sk->sk_receive_queue;
	__skb_queue_after(&sk->sk_receive_queue, last, skb);
	quic_inq(sk)->last_event = skb;
	sk->sk_data_ready(sk);
	return 0;
}
