/* sysdep_pfpacket.c - Tracing of forwarded packets using PF_PACKET
 *
 * Copyright (C) 2007 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later as
 * published by the Free Software Foundation.
 *
 * See http://www.gnu.org/ for details.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>

#include "nhrp_defines.h"
#include "nhrp_common.h"
#include "nhrp_interface.h"
#include "nhrp_peer.h"

#define MAX_OPCODES 100

static int packet_fd;

enum {
	LABEL_NEXT = 0,
	LABEL_SKIP1,
	LABEL_SKIPN,
	LABEL_DROP,
	LABEL_ACCEPT,
	LABEL_IF_OK,
	LABEL_NOT_IPV4,
	NUM_LABELS
};

struct filter {
	int pos[NUM_LABELS];
	int numops;
	struct sock_filter code[MAX_OPCODES];
};

static void emit_stmt(struct filter *f, __u16 code, __u32 k)
{
	if (f->numops < MAX_OPCODES) {
		f->code[f->numops].code = code;
		f->code[f->numops].jt = 0;
		f->code[f->numops].jf = 0;
		f->code[f->numops].k = k;
	}
	f->numops++;
}

static void emit_jump(struct filter *f, __u16 code, __u32 k, __u8 jt, __u8 jf)
{
	if (f->numops < MAX_OPCODES) {
		f->code[f->numops].code = code;
		f->code[f->numops].jt = jt;
		f->code[f->numops].jf = jf;
		f->code[f->numops].k = k;
	}
	f->numops++;
}

static int patch_jump(struct filter *f, int new_label)
{
	if (BPF_CLASS(f->code[f->numops-1].code) != BPF_JMP)
		return FALSE;

	if (f->code[f->numops-1].jf == LABEL_NEXT)
		f->code[f->numops-1].jf = new_label;
	if (f->code[f->numops-1].jt == LABEL_NEXT)
		f->code[f->numops-1].jt = new_label;
	return TRUE;
}

static void mark(struct filter *f, int label)
{
	f->pos[label] = f->numops;
}

static int check_interface(void *ctx, struct nhrp_interface *iface)
{
	struct filter *f = (struct filter *) ctx;

	emit_jump(f, BPF_JMP|BPF_JEQ|BPF_K, iface->index, LABEL_IF_OK, LABEL_NEXT);

	return 0;
}

static int check_ipv4(void *ctx, struct nhrp_peer *peer)
{
	struct filter *f = (struct filter *) ctx;
	unsigned long addr, mask;

	if (peer->protocol_type != ETHPROTO_IP)
		return 0;

	addr = htonl(*((unsigned long *) peer->protocol_address.addr));
	if (peer->prefix_length != 32) {
		mask = 0xffffffff >> peer->prefix_length;
		emit_jump(f, BPF_JMP|BPF_JGE|BPF_K, addr & ~mask, LABEL_NEXT, LABEL_SKIP1);
		emit_jump(f, BPF_JMP|BPF_JGT|BPF_K, addr |  mask, LABEL_NEXT, LABEL_DROP);
	} else {
		emit_jump(f, BPF_JMP|BPF_JEQ|BPF_K, addr, LABEL_DROP, LABEL_NEXT);
	}
	return 0;
}

static struct nhrp_task install_filter_task;
NHRP_TASK(install_filter);

void install_filter_callback(struct nhrp_task *task)
{
	struct nhrp_peer_selector sel;
	struct sock_fprog prog;
	struct filter f;
	int i;

	memset(&f, 0, sizeof(f));

	/* First, we are interested only on outgoing stuff */
	emit_stmt(&f, BPF_LD |BPF_W  |BPF_ABS, SKF_AD_OFF+SKF_AD_PKTTYPE);
	emit_jump(&f, BPF_JMP|BPF_JEQ|BPF_K,   PACKET_OUTGOING, LABEL_NEXT, LABEL_DROP);

	/* Check for valid interface */
	emit_stmt(&f, BPF_LD |BPF_W  |BPF_ABS, SKF_AD_OFF+SKF_AD_IFINDEX);
	nhrp_interface_foreach(check_interface, &f);
	if (!patch_jump(&f, LABEL_DROP))
		return;
	mark(&f, LABEL_IF_OK);

	/* Check for IPv4 */
	emit_stmt(&f, BPF_LD |BPF_W  |BPF_ABS, SKF_AD_OFF+SKF_AD_PROTOCOL);
	emit_jump(&f, BPF_JMP|BPF_JEQ|BPF_K,   ETH_P_IP, LABEL_NEXT, LABEL_NOT_IPV4);

	/* Check for multicast IPv4 destination */
	emit_stmt(&f, BPF_LD |BPF_W  |BPF_ABS, offsetof(struct iphdr, daddr));
	emit_jump(&f, BPF_JMP|BPF_JGE|BPF_K, 0xe0000000, LABEL_NEXT, LABEL_SKIP1);
	emit_jump(&f, BPF_JMP|BPF_JGE|BPF_K, 0xf0000000, LABEL_NEXT, LABEL_ACCEPT);

	/* Check for non-local IPv4 source */
	emit_stmt(&f, BPF_LD |BPF_W  |BPF_ABS, offsetof(struct iphdr, saddr));

	memset(&sel, 0, sizeof(sel));
	sel.type_mask = BIT(NHRP_PEER_TYPE_LOCAL);
	nhrp_peer_foreach(check_ipv4, &f, &sel);
	emit_stmt(&f, BPF_RET|BPF_K, 65535);

	mark(&f, LABEL_NOT_IPV4);

	/* Exit */
	mark(&f, LABEL_DROP);
	emit_stmt(&f, BPF_RET|BPF_K, 0);
	mark(&f, LABEL_ACCEPT);
	emit_stmt(&f, BPF_RET|BPF_K, 65535);

	/* All ok so far? */
	if (f.numops >= MAX_OPCODES) {
		nhrp_error("Filter code buffer too small (code actual length %d)",
			   f.numops);
		return;
	}

	/* Fixup jumps to be relative */
	for (i = 0; i < f.numops; i++) {
		if (BPF_CLASS(f.code[i].code) == BPF_JMP) {
			if (f.code[i].jt > LABEL_SKIPN)
				f.code[i].jt = f.pos[f.code[i].jt] - i - 1;
			if (f.code[i].jf > LABEL_SKIPN)
				f.code[i].jf = f.pos[f.code[i].jf] - i - 1;
		}
	}

	/* Attach filter */
	prog.len = f.numops;
	prog.filter = f.code;
	if (setsockopt(packet_fd, SOL_SOCKET, SO_ATTACH_FILTER,
		       &prog, sizeof(prog)))
		return;

	nhrp_info("Filter code installed (%d opcodes)", f.numops);
	nhrp_task_cancel(&install_filter_task);
}

char *install_filter_describe(struct nhrp_task *task, size_t buflen, char *buf)
{
	snprintf(buf, buflen, "Install PF_PACKET filter code");
	return buf;
}

int forward_local_addresses_changed(void)
{
	nhrp_task_schedule(&install_filter_task, 0, &install_filter);
	return TRUE;
}

static int pfp_send_mcast(void *ctx, struct nhrp_peer *peer)
{
	struct msghdr *msg = (struct msghdr *) ctx;
	struct sockaddr_ll *lladdr = (struct sockaddr_ll *) msg->msg_name;
	char to[32];

	nhrp_debug("Sending multicast to nbma %s",
		   nhrp_address_format(&peer->next_hop_address,
				       sizeof(to), to));

	lladdr->sll_halen = peer->next_hop_address.addr_len;
	memcpy(lladdr->sll_addr, peer->next_hop_address.addr,
	       lladdr->sll_halen);

	if (sendmsg(packet_fd, msg, 0) < 0) {
		nhrp_error("Failed to forward multicast packet to %s",
			   nhrp_address_format(&peer->next_hop_address,
					       sizeof(to), to));
	}

	return 0;
}

static int pfp_read(void *ctx, int fd, short events)
{
	struct nhrp_interface *iface;
	struct nhrp_peer_selector sel;
	struct nhrp_address src, dst;
	struct sockaddr_ll lladdr;
	struct iovec iov;
	struct msghdr msg = {
		.msg_name = &lladdr,
		.msg_namelen = sizeof(lladdr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	union {
		uint8_t buf[2048];
		struct iphdr iphdr;
	} u;
	char fr[32], to[32];
	int status, i, nbmaset;

	if (!(events & POLLIN))
		return 0;

	iov.iov_base = u.buf;
	while (TRUE) {
		iov.iov_len = sizeof(u.buf);
		iov.iov_len = status = recvmsg(fd, &msg, MSG_DONTWAIT);
		msg.msg_namelen = sizeof(lladdr);
		if (status < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 0;
			nhrp_perror("PF_PACKET overrun");
			continue;
		}

		if (iov.iov_len == 0) {
			nhrp_error("PF_PACKET returned EOF");
			return 0;
		}

		if (lladdr.sll_pkttype != PACKET_OUTGOING)
			continue;

		iface = nhrp_interface_get_by_index(lladdr.sll_ifindex, FALSE);
		if (iface == NULL)
			continue;

		if (!nhrp_address_parse_packet(lladdr.sll_protocol,
					       iov.iov_len, u.buf,
					       &src, &dst))
			return FALSE;

		nbmaset = 0;
		for (i = 0; i < lladdr.sll_halen; i++)
			if (lladdr.sll_addr[i] != 0)
				nbmaset = 1;

		if (nhrp_address_is_multicast(&dst)) {
			if (nbmaset)
				continue;

			nhrp_debug("Multicast from %s to %s",
				   nhrp_address_format(&src, sizeof(fr), fr),
				   nhrp_address_format(&dst, sizeof(to), to));


			memset(&sel, 0, sizeof(sel));
			sel.interface = iface;
			sel.type_mask =
				BIT(NHRP_PEER_TYPE_STATIC) |
				BIT(NHRP_PEER_TYPE_DYNAMIC) |
				BIT(NHRP_PEER_TYPE_CACHED);
			sel.flags = NHRP_PEER_FLAG_UP;
			nhrp_peer_foreach(pfp_send_mcast, &msg, &sel);
		} else {
			nhrp_packet_send_traffic(iface, lladdr.sll_protocol,
						 u.buf, iov.iov_len);
		}
	}

	return 0;
}

int forward_init(void)
{
	packet_fd = socket(PF_PACKET, SOCK_DGRAM, ntohs(ETH_P_ALL));
	if (packet_fd < 0) {
		nhrp_error("Unable to create PF_PACKET socket");
		return FALSE;
	}

	fcntl(packet_fd, F_SETFD, FD_CLOEXEC);
	install_filter_callback(&install_filter_task);

	if (!nhrp_task_poll_fd(packet_fd, POLLIN, pfp_read, NULL))
		goto err_close;

	return TRUE;

err_close:
	close(packet_fd);
	return FALSE;
}

