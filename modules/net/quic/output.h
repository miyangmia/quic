/* SPDX-License-Identifier: GPL-2.0-or-later */
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

struct quic_outqueue {
	struct list_head packet_sent_list;
	struct list_head transmitted_list;
	struct list_head datagram_list;
	struct list_head control_list;
	struct list_head stream_list;
	struct work_struct work;
	u64 last_max_bytes;
	u64 max_bytes;
	u64 max_data;
	u64 bytes;

	u16 max_datagram_frame_size;
	u16 max_udp_payload_size;
	u8 ack_delay_exponent;
	u32 max_idle_timeout;
	u32 stream_list_len;	/* all frames len in stream list */
	u32 max_ack_delay;
	u32 inflight;		/* all inflight ack_eliciting frames len */
	u32 window;
	u16 count;

	/* Use for 0-RTT/1-RTT DATA (re)transmit,
	 * as QUIC_SKB_CB(skb)->level is always QUIC_CRYPTO_APP.
	 * Set this level to QUIC_CRYPTO_EARLY or QUIC_CRYPTO_APP
	 * when the corresponding crypto is ready for send.
	 */
	u8 data_level;
	u8 *close_phrase;
	u32 close_errcode;
	u8 close_frame;
	u8 pto_count;

	u8 disable_compatible_version:1;
	u8 disable_1rtt_encryption:1;
	u8 grease_quic_bit:1;
	u8 stateless_reset:1;
	u8 token_pending:1;
	u8 data_blocked:1;
	u8 force_delay:1;
	u8 single:1;
};

static inline void quic_outq_inc_inflight(struct quic_outqueue *outq, u32 len)
{
	outq->inflight += len;
}

static inline u32 quic_outq_inflight(struct quic_outqueue *outq)
{
	return outq->inflight;
}

static inline u64 quic_outq_window(struct quic_outqueue *outq)
{
	return outq->window;
}

static inline u8 quic_outq_ack_delay_exponent(struct quic_outqueue *outq)
{
	return outq->ack_delay_exponent;
}

static inline u16 quic_outq_max_udp(struct quic_outqueue *outq)
{
	return outq->max_udp_payload_size;
}

static inline u64 quic_outq_max_bytes(struct quic_outqueue *outq)
{
	return outq->max_bytes;
}

static inline void quic_outq_set_max_bytes(struct quic_outqueue *outq, u64 bytes)
{
	outq->max_bytes = bytes;
}

static inline u32 quic_outq_close_errcode(struct quic_outqueue *outq)
{
	return outq->close_errcode;
}

static inline void quic_outq_set_close_errcode(struct quic_outqueue *outq, u32 errcode)
{
	outq->close_errcode = errcode;
}

static inline u8 quic_outq_close_frame(struct quic_outqueue *outq)
{
	return outq->close_frame;
}

static inline void quic_outq_set_close_frame(struct quic_outqueue *outq, u8 type)
{
	outq->close_frame = type;
}

static inline u8 *quic_outq_close_phrase(struct quic_outqueue *outq)
{
	return outq->close_phrase;
}

static inline void quic_outq_set_close_phrase(struct quic_outqueue *outq, u8 *phrase)
{
	outq->close_phrase = phrase;
}

static inline u16 quic_outq_max_dgram(struct quic_outqueue *outq)
{
	return outq->max_datagram_frame_size;
}

static inline u8 quic_outq_grease_quic_bit(struct quic_outqueue *outq)
{
	return outq->grease_quic_bit;
}

static inline void quic_outq_set_data_level(struct quic_outqueue *outq, u8 level)
{
	outq->data_level = level;
}

static inline void quic_outq_set_force_delay(struct quic_outqueue *outq, u8 delay)
{
	outq->force_delay = !!delay;
}

static inline u8 quic_outq_token_pending(struct quic_outqueue *outq)
{
	return outq->token_pending;
}

static inline void quic_outq_set_token_pending(struct quic_outqueue *outq, u8 pending)
{
	outq->token_pending = pending;
}

static inline void quic_outq_set_data_blocked(struct quic_outqueue *outq, u8 blocked)
{
	outq->data_blocked = blocked;
}

void quic_outq_stream_tail(struct sock *sk, struct quic_frame *frame, bool cork);
void quic_outq_dgram_tail(struct sock *sk, struct quic_frame *frame, bool cork);
void quic_outq_ctrl_tail(struct sock *sk, struct quic_frame *frame, bool cork);
void quic_outq_update_path(struct sock *sk, u8 path);
void quic_outq_transmit_pto(struct sock *sk);

int quic_outq_transmit_frame(struct sock *sk, u8 type, void *data, u8 path, u8 cork);
int quic_outq_transmit_retire_conn_id(struct sock *sk, u64 prior, u8 path, u8 cork);
int quic_outq_transmit_new_conn_id(struct sock *sk, u64 prior, u8 path, u8 cork);
int quic_outq_stream_append(struct sock *sk, struct quic_msginfo *info, u8 pack);
int quic_outq_probe_path_alt(struct sock *sk, u8 cork);
int quic_outq_transmit(struct sock *sk);

void quic_outq_transmitted_sack(struct sock *sk, u8 level, s64 largest,
				s64 smallest, s64 ack_largest, u32 ack_delay);
void quic_outq_packet_sent_tail(struct sock *sk, struct quic_packet_sent *info);
void quic_outq_transmitted_tail(struct sock *sk, struct quic_frame *frame);
void quic_outq_retransmit_list(struct sock *sk, struct list_head *head);
void quic_outq_retransmit_mark(struct sock *sk, u8 level, u8 immediate);
void quic_outq_update_loss_timer(struct sock *sk);

void quic_outq_transmit_close(struct sock *sk, u8 frame, u32 errcode, u8 level);
void quic_outq_stream_list_purge(struct sock *sk, struct quic_stream *stream);
void quic_outq_encrypted_tail(struct sock *sk, struct sk_buff *skb);
void quic_outq_transmit_app_close(struct sock *sk);
void quic_outq_transmit_probe(struct sock *sk);

void quic_outq_get_param(struct sock *sk, struct quic_transport_param *p);
void quic_outq_set_param(struct sock *sk, struct quic_transport_param *p);
void quic_outq_sync_window(struct sock *sk, u32 window);
void quic_outq_init(struct sock *sk);
void quic_outq_free(struct sock *sk);

int quic_outq_flow_control(struct sock *sk, struct quic_stream *stream, u16 bytes, u8 sndblock);
u64 quic_outq_wspace(struct sock *sk, struct quic_stream *stream);
