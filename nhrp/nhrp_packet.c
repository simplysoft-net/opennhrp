/* nhrp_packet.c - NHRP packet marshalling and tranceiving
 *
 * Copyright (C) 2007 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <netinet/in.h>
#include "nhrp_packet.h"

#define MAX_PDU_SIZE 1500

struct nhrp_buffer *nhrp_buffer_alloc(uint32_t size)
{
	struct nhrp_buffer *buf;

	buf = malloc(sizeof(struct nhrp_buffer) + size);
	buf->length = size;

	return buf;
}

void nhrp_buffer_free(struct nhrp_buffer *buffer)
{
	free(buffer);
}

struct nhrp_packet *nhrp_packet_alloc(void)
{
	return calloc(1, sizeof(struct nhrp_packet));
}

void nhrp_packet_free(struct nhrp_packet *packet)
{
	free(packet);
}

int nhrp_packet_recv(struct nhrp_packet *packet)
{
	return FALSE;
}

static int marshall_binary(uint8_t **pdu, size_t *pduleft, size_t size, void *raw)
{
	if (*pduleft < size)
		return FALSE;

	memcpy(*pdu, raw, size);
	*pdu += size;
	*pduleft -= size;

	return TRUE;
}

static inline int marshall_protocol_address(uint8_t **pdu, size_t *pduleft, struct nhrp_protocol_address *pa)
{
	return marshall_binary(pdu, pduleft, pa->addr_len, pa->addr);
}

static inline int marshall_nbma_address(uint8_t **pdu, size_t *pduleft, struct nhrp_nbma_address *na)
{
	if (!marshall_binary(pdu, pduleft, na->addr_len, na->addr))
		return FALSE;

	return marshall_binary(pdu, pduleft, na->subaddr_len, na->subaddr);
}

static int marshall_payload(uint8_t **pdu, size_t *pduleft, struct nhrp_payload *p)
{
	switch (p->type) {
	case NHRP_PAYLOAD_TYPE_NONE:
		return TRUE;
	case NHRP_PAYLOAD_TYPE_RAW:
		return marshall_binary(pdu, pduleft, p->u.raw->length, p->u.raw->data);
	default:
		return FALSE;
	}
}

static int marshall_packet(uint8_t *pdu, size_t pduleft, struct nhrp_packet *packet)
{
	uint8_t *pos = pdu;
	struct nhrp_packet_header *phdr = (struct nhrp_packet_header *) pdu;
	struct nhrp_extension_header neh;
	int i;

	if (!marshall_binary(&pos, &pduleft, sizeof(packet->hdr), &packet->hdr))
		return -1;
	if (!marshall_nbma_address(&pos, &pduleft, &packet->src_nbma_address))
		return -1;
	if (!marshall_protocol_address(&pos, &pduleft, &packet->src_protocol_address))
		return -1;
	if (!marshall_protocol_address(&pos, &pduleft, &packet->dst_protocol_address))
		return -1;
	if (!marshall_payload(&pos, &pduleft, &packet->extension[NHRP_EXTENSION_PAYLOAD]))
		return -1;

	phdr->extension_offset = htons((int)(pos - pdu));
	for (i = 1; i < ARRAY_SIZE(packet->extension); i++) {
		struct nhrp_extension_header *eh = (struct nhrp_extension_header *) pos;

		if (packet->extension[i].type == NHRP_PAYLOAD_TYPE_NONE)
			continue;

		neh.type = htons(i);
		if (packet->extension[i].flags & NHRP_PAYLOAD_FLAG_COMPULSORY)
			neh.type |= NHRP_EXTENSION_FLAG_COMPULSORY;
		neh.length = 0;

		if (!marshall_binary(&pos, &pduleft, sizeof(neh), &neh))
			return -1;
		if (!marshall_payload(&pos, &pduleft, &packet->extension[i]))
			return -1;
		eh->length = htons((pos - (uint8_t *) eh) - sizeof(neh));
	}
	neh.type = htons(NHRP_EXTENSION_END) | NHRP_EXTENSION_FLAG_COMPULSORY;
	neh.length = 0;
	if (!marshall_binary(&pos, &pduleft, sizeof(neh), &neh))
		return -1;

	phdr->packet_size = htons((int)(pos - pdu));
	phdr->checksum = 0;
	phdr->src_nbma_address_len = packet->src_nbma_address.addr_len;
	phdr->src_nbma_subaddress_len = packet->src_nbma_address.subaddr_len;
	phdr->src_protocol_address_len = packet->src_protocol_address.addr_len;
	phdr->dst_protocol_address_len = packet->dst_protocol_address.addr_len;

	return (int)(pos - pdu);
}

static void hex_dump(const char *name, const uint8_t *buf, int bytes)
{
	int i, j;
	int left;

	fprintf(stderr, "%s:\n", name);
	for (i = 0; i < bytes; i++) {
		fprintf(stderr, "%02X ", buf[i]);
		if (i % 0x10 == 0x0f) {
			fprintf(stderr, "    ");
			for (j = 0; j < 0x10; j++)
				fprintf(stderr, "%c", isgraph(buf[i+j-0xf]) ?
					buf[i+j-0xf]: '.');
			fprintf(stderr, "\n");
		}
	}

	left = i % 0x10;
	if (left != 0) {
		fprintf(stderr, "%*s    ", 3 * (0x10 - left), "");

		for (j = 0; j < left; j++)
			fprintf(stderr, "%c", isgraph(buf[i+j-left]) ?
				buf[i+j-left]: '.');
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "\n");
}

int nhrp_packet_send(struct nhrp_packet *packet)
{
	uint8_t pdu[MAX_PDU_SIZE];
	int size;

	size = marshall_packet(pdu, sizeof(pdu), packet);
	if (size < 0)
		return FALSE;

	hex_dump("packet", pdu, size);

	return FALSE;
}
