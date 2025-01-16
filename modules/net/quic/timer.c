// SPDX-License-Identifier: GPL-2.0-or-later
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include "socket.h"

void quic_timer_sack_handler(struct sock *sk)
{
	struct quic_pnspace *space = quic_pnspace(sk, QUIC_CRYPTO_APP);
	struct quic_inqueue *inq = quic_inq(sk);
	struct quic_connection_close *close;
	u8 buf[100] = {};

	if (quic_is_closed(sk))
		return;

	if (quic_inq_need_sack(inq)) {
		if (quic_inq_need_sack(inq) == 2) {
			quic_pnspace_set_need_sack(space, 1);
			quic_pnspace_set_path_alt(space, 0);
		}

		quic_outq_transmit(sk);
		quic_inq_set_need_sack(inq, 0);

		quic_timer_start(sk, QUIC_TIMER_IDLE, quic_inq_max_idle_timeout(inq));
		return;
	}

	close = (void *)buf;
	quic_inq_event_recv(sk, QUIC_EVENT_CONNECTION_CLOSE, close);
	quic_set_state(sk, QUIC_SS_CLOSED);
	pr_debug("%s: idle timeout\n", __func__);
}

static void quic_timer_sack_timeout(struct timer_list *t)
{
	struct quic_sock *qs = from_timer(qs, t, timers[QUIC_TIMER_SACK].t);
	struct sock *sk = &qs->inet.sk;

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		if (!test_and_set_bit(QUIC_SACK_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
		goto out;
	}

	quic_timer_sack_handler(sk);
out:
	bh_unlock_sock(sk);
	sock_put(sk);
}

void quic_timer_loss_handler(struct sock *sk)
{
	if (quic_is_closed(sk))
		return;

	quic_outq_transmit_pto(sk);
}

static void quic_timer_loss_timeout(struct timer_list *t)
{
	struct quic_sock *qs = from_timer(qs, t, timers[QUIC_TIMER_LOSS].t);
	struct sock *sk = &qs->inet.sk;

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		if (!test_and_set_bit(QUIC_LOSS_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
		goto out;
	}

	quic_timer_loss_handler(sk);
out:
	bh_unlock_sock(sk);
	sock_put(sk);
}

void quic_timer_path_handler(struct sock *sk)
{
	struct quic_path_addr *path;
	struct quic_frame *frame;
	u8 cnt, probe = 1;
	u32 timeout;

	if (quic_is_closed(sk))
		return;

	timeout = quic_cong_pto(quic_cong(sk)) * 3;
	path = quic_src(sk);
	cnt = quic_path_sent_cnt(path);
	if (cnt) {
		probe = 0;
		if (cnt >= 5) {
			quic_path_set_sent_cnt(path, 0);
			return;
		}
		frame = quic_frame_create(sk, QUIC_FRAME_PATH_CHALLENGE, path);
		if (frame)
			quic_outq_ctrl_tail(sk, frame, false);
		quic_path_set_sent_cnt(path, cnt + 1);
		quic_timer_start(sk, QUIC_TIMER_PATH, timeout);
	}

	path = quic_dst(sk);
	cnt = quic_path_sent_cnt(path);
	if (cnt) {
		probe = 0;
		if (cnt >= 5) {
			quic_path_set_sent_cnt(path, 0);
			quic_path_swap_active(path);
			return;
		}
		frame = quic_frame_create(sk, QUIC_FRAME_PATH_CHALLENGE, path);
		if (frame)
			quic_outq_ctrl_tail(sk, frame, false);
		quic_path_set_sent_cnt(path, cnt + 1);
		quic_timer_start(sk, QUIC_TIMER_PATH, timeout);
	}

	if (probe)
		quic_outq_transmit_probe(sk);
}

static void quic_timer_path_timeout(struct timer_list *t)
{
	struct quic_sock *qs = from_timer(qs, t, timers[QUIC_TIMER_PATH].t);
	struct sock *sk = &qs->inet.sk;

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		if (!test_and_set_bit(QUIC_PATH_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
		goto out;
	}

	quic_timer_path_handler(sk);
out:
	bh_unlock_sock(sk);
	sock_put(sk);
}

void quic_timer_pace_handler(struct sock *sk)
{
	if (quic_is_closed(sk))
		return;
	quic_outq_transmit(sk);
}

static enum hrtimer_restart quic_timer_pace_timeout(struct hrtimer *hr)
{
	struct quic_sock *qs = container_of(hr, struct quic_sock, timers[QUIC_TIMER_PACE].hr);
	struct sock *sk = &qs->inet.sk;

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		if (!test_and_set_bit(QUIC_TSQ_DEFERRED, &sk->sk_tsq_flags))
			sock_hold(sk);
		goto out;
	}

	quic_timer_pace_handler(sk);
out:
	bh_unlock_sock(sk);
	sock_put(sk);
	return HRTIMER_NORESTART;
}

void quic_timer_reset(struct sock *sk, u8 type, u64 timeout)
{
	struct timer_list *t = quic_timer(sk, type);

	if (timeout && !mod_timer(t, jiffies + usecs_to_jiffies(timeout)))
		sock_hold(sk);
}

void quic_timer_start(struct sock *sk, u8 type, u64 timeout)
{
	struct timer_list *t;
	struct hrtimer *hr;

	if (type == QUIC_TIMER_PACE) {
		hr = quic_timer(sk, type);

		if (!hrtimer_is_queued(hr)) {
			hrtimer_start(hr, ns_to_ktime(timeout), HRTIMER_MODE_ABS_PINNED_SOFT);
			sock_hold(sk);
		}
		return;
	}

	t = quic_timer(sk, type);
	if (timeout && !timer_pending(t)) {
		if (!mod_timer(t, jiffies + usecs_to_jiffies(timeout)))
			sock_hold(sk);
	}
}

void quic_timer_stop(struct sock *sk, u8 type)
{
	if (type == QUIC_TIMER_PACE)
		return;
	if (del_timer(quic_timer(sk, type)))
		sock_put(sk);
}

void quic_timer_init(struct sock *sk)
{
	struct hrtimer *hr;

	timer_setup(quic_timer(sk, QUIC_TIMER_LOSS), quic_timer_loss_timeout, 0);
	timer_setup(quic_timer(sk, QUIC_TIMER_SACK), quic_timer_sack_timeout, 0);
	timer_setup(quic_timer(sk, QUIC_TIMER_PATH), quic_timer_path_timeout, 0);

	hr = quic_timer(sk, QUIC_TIMER_PACE);
	hrtimer_init(hr, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED_SOFT);
	hr->function = quic_timer_pace_timeout;
}

void quic_timer_free(struct sock *sk)
{
	quic_timer_stop(sk, QUIC_TIMER_LOSS);
	quic_timer_stop(sk, QUIC_TIMER_SACK);
	quic_timer_stop(sk, QUIC_TIMER_PATH);
	quic_timer_stop(sk, QUIC_TIMER_PACE);
}
