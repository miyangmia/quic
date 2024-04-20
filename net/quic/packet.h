/* SPDX-License-Identifier: GPL-2.0-or-later */
/* QUIC kernel implementation
 * (C) Copyright Red Hat Corp. 2023
 *
 * This file is part of the QUIC kernel implementation
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

struct quic_packet {
	struct sk_buff_head frame_list;
	struct sk_buff *head;
	union quic_addr *da;
	union quic_addr *sa;
	u32 overhead;
	u32 len;

	u32 mss[2];
	u16 max_count;
	u16 count;
	u8  level;

	u8  ecn_probes;
	u8  ipfragok:1;
	u8  path_alt:2;
	u8  padding:1;
	u8  filter:1;
	u8  taglen[2];
};

#define QUIC_PACKET_INITIAL_V1		0
#define QUIC_PACKET_0RTT_V1		1
#define QUIC_PACKET_HANDSHAKE_V1	2
#define QUIC_PACKET_RETRY_V1		3

#define QUIC_PACKET_INITIAL_V2		1
#define QUIC_PACKET_0RTT_V2		2
#define QUIC_PACKET_HANDSHAKE_V2	3
#define QUIC_PACKET_RETRY_V2		0

#define QUIC_PACKET_INITIAL		QUIC_PACKET_INITIAL_V1
#define QUIC_PACKET_0RTT		QUIC_PACKET_0RTT_V1
#define QUIC_PACKET_HANDSHAKE		QUIC_PACKET_HANDSHAKE_V1
#define QUIC_PACKET_RETRY		QUIC_PACKET_RETRY_V1

struct quic_request_sock;

static inline u32 quic_packet_taglen(struct quic_packet *packet)
{
	return packet->taglen[0];
}

static inline u32 quic_packet_mss(struct quic_packet *packet)
{
	return packet->mss[0] - packet->taglen[!!packet->level];
}

static inline u32 quic_packet_overhead(struct quic_packet *packet)
{
	return packet->overhead;
}

static inline u32 quic_packet_max_payload(struct quic_packet *packet)
{
	return packet->mss[0] - packet->overhead - packet->taglen[!!packet->level];
}

static inline u32 quic_packet_max_payload_dgram(struct quic_packet *packet)
{
	return packet->mss[1] - packet->overhead - packet->taglen[!!packet->level];
}

static inline void quic_packet_set_taglen(struct quic_packet *packet, u8 taglen)
{
	packet->taglen[0] = taglen;
}

static inline void quic_packet_set_ecn_probes(struct quic_packet *packet, u8 probes)
{
	packet->ecn_probes = probes;
}

void quic_packet_set_filter(struct sock *sk, u8 level, u16 count);
void quic_packet_build(struct sock *sk);
int quic_packet_config(struct sock *sk, u8 level, u8 path_alt);
int quic_packet_route(struct sock *sk);
int quic_packet_process(struct sock *sk, struct sk_buff *skb, u8 resume);
int quic_packet_tail(struct sock *sk, struct sk_buff *skb, struct sk_buff_head *from, u8 dgram);
int quic_packet_flush(struct sock *sk);
int quic_packet_retry_transmit(struct sock *sk, struct quic_request_sock *req);
int quic_packet_version_transmit(struct sock *sk, struct quic_request_sock *req);
int quic_packet_stateless_reset_transmit(struct sock *sk, struct quic_request_sock *req);
int quic_packet_refuse_close_transmit(struct sock *sk, struct quic_request_sock *req, u32 errcode);
int quic_packet_early_decrypt(struct sk_buff *skb);
void quic_packet_mss_update(struct sock *sk, int mss);
int quic_packet_xmit(struct sock *sk, struct sk_buff *skb, u8 resume);
void quic_packet_init(struct sock *sk);
