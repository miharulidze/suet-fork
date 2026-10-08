/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2015-2018 Intel Corporation. All rights reserved.
 * Copyright (c) 2026 ETH Zurich. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <ofi.h>
#include <ofi_proto.h>

#ifndef _SUET_PROTO_H_
#define _SUET_PROTO_H_

/*
 * UET AV Address Format (based partially on UET Spec Table 2-10).
 *
 * Wraps the underlying datagram-layer address with UET-specific
 * addressing fields (PIDonFEP, Resource Index, etc.).  This is
 * the address format returned by provider's fi_getname() and accepted by
 * fi_av_insert().
 */

#define SUET_ADDR_VERSION 1
#define SUET_DGRAM_AV_NAME_LENGTH 32

struct suet_av_addr {
    uint16_t version;          /* protocol version (SUET_AV_ADDR_VERSION) */
    uint16_t flags;            /* address flags */
    uint16_t fep_caps;         /* FEP capabilities (bit 7 = optimized hdr) */
    uint16_t pid_on_fep;       /* PIDonFEP (12-bit significant) */
    uint16_t start_ri;         /* starting resource index (12-bit) */
    uint16_t num_ri;           /* number of RIs (12-bit) */
    uint32_t initiator_id;     /* initiator ID for matching */
    uint8_t raw_dgram_addr[SUET_DGRAM_AV_NAME_LENGTH]; /* underlying datagram
							  address */
    uint16_t raw_dgram_addrlen; /* length of raw_dgram_addr in bytes */
};

/*
 *
 * PDS (Packet Delivery Sublayer) Protocol Definitions (Section 3.3)
 * 
 */

#define PDS_MAX_MTU_SIZE     4096
#define PDS_MAX_PDCID        UINT16_MAX
#define PDS_MIN_PSN_DISTANCE (1U << 16) /* Section 3.5.8.2: min circular distance */

/* PDS prologue (16 bits). */
union pds_prologue {
    uint16_t raw;
    /* DO NOT ACCESS -- Union members below are solely for the documentation purpose. */
    struct {
        uint16_t flags    : 7; /* [6:0]   transport flags */
        uint16_t next_hdr : 4; /* [10:7]  SES header type (Table 3-16) */
        uint16_t type     : 5; /* [15:11] packet type (Table 3-32) */
    } bits;
};

/* PDS dpdcid / SYN overlay (16 bits). */
union pds_dpdcid {
    uint16_t raw;
    /* DO NOT ACCESS -- Union members below are solely for the documentation purpose. */
    uint16_t dpdcid;                   /* destination PDCID (flags.syn=0) */
    struct {                           /* flags.syn=1 overlay (Table 3-33) */
        uint16_t syn_psn_offset  : 12; /* [11:0]  starting PSN offset */
        uint16_t syn_rsvd        : 3;  /* [14:12] reserved */
        uint16_t syn_use_rsv_pdc : 1;  /* [15]    use reserved PDC */
    } syn_bits;
};

/*
 * RUD/ROD Request header (Section 3.5.10, Table 3-33).
 * 12 bytes, matching the spec's uet_pds_rud_rod_request layout.
 */
struct __attribute__((packed)) pds_req_hdr {
    union pds_prologue prologue;
    uint16_t clear_psn_offset; /* PSN - CLEAR_PSN (Table 3-33) */
    uint32_t psn;        /* packet sequence number */
    uint16_t spdcid;     /* source PDCID */
    union pds_dpdcid dpdcid;
};

/*
 * PDS ACK header (Section 3.5.10, Table 3-35).
 * 12 bytes, matching the spec's uet_pds_ack layout.
 * Same wire layout as pds_req_hdr but with different field semantics.
 */
struct __attribute__((packed)) pds_ack_hdr {
    union pds_prologue prologue;
    uint16_t ack_psn_offset;
    uint32_t cack_psn;   /* cumulative ACK PSN */
    uint16_t spdcid;     /* source PDCID */
    uint16_t dpdcid;     /* destination PDCID */
};

/* ACK_CC / ACK_CCX: Spec Tables 3-36 and 3-37. Network byte order. */
struct __attribute__((packed)) pds_ack_cc_hdr {
	struct pds_ack_hdr ack;
	uint8_t cc_type_flags;
	uint8_t mpr;
	int16_t sack_psn_offset;
	uint64_t sack_bitmap;
	uint64_t ack_cc_state;
};

/* PDS packet type codes -- pds.type prologue field (Section 3.5.10.2, Table 3-32).
 * RUD and ROD share a single Control Packet type (PDS_CP); the RUD/ROD
 * distinction is carried per-CP in the isrod flag (only meaningful for
 * NOOP and Negotiation CPs per Table 3-38). The CP subtype lives in the
 * prologue's next_hdr nibble as ctl_type. */
enum pds_pkt_type {
	PDS_RUD_REQ = 2, /* RUD Request: carries SES payload (Table 3-33) */
	PDS_ROD_REQ = 3, /* ROD Request: carries SES payload (Table 3-33) */
	PDS_ACK = 7, /* Acknowledgement: cumulative ack + SES response (Table
			3-35) */
	PDS_ACK_CC = 8,
	PDS_ACK_CCX = 9,
	PDS_NACK = 10, /* Negative Acknowledgement (Table 3-32) */
	PDS_CP = 11, /* Control Packet (Section 3.5.10.8 / 3.5.16); subtype in
			ctl_type */
};

static inline const char *pds_pkt_type_name(int type)
{
    switch (type) {
    case PDS_RUD_REQ:
        return "PDS_RUD_REQ";
    case PDS_ROD_REQ:
        return "PDS_ROD_REQ";
    case PDS_CP:
        return "PDS_CP";
    case PDS_ACK:
        return "PDS_ACK";
    case PDS_NACK:
        return "PDS_NACK";
    default:
        return "PDS_UNKNOWN";
    }
}

/* pds.flags bit assignments -- within the 7-bit flags field of the prologue
 * (Section 3.5.11, Table 3-33 for RUD/ROD request; Table 3-38 for CP).
 * Bit numbering within the flags[6:0] sub-field of the 16-bit prologue:
 *   prologue bit  [5]   = ISROD (CP only)
 *   prologue bit  [4]   = RETX
 *   prologue bit  [3]   = AR
 *   prologue bit  [2]   = SYN
 *   other bits          = reserved
 */
#define PDS_FLAG_SYN   (1 << 2) /* [2] PDC establishment request (Section 3.5.8.2) */
#define PDS_FLAG_AR    (1 << 3) /* [3] ACK requested */
#define PDS_FLAG_RETX  (1 << 4) /* [4] retransmitted packet */
#define PDS_FLAG_ISROD (1 << 5) /* [5] CP only (Table 3-38): 1 => PDC is ROD */
#define PDS_ACK_FLAG_M (1 << 5) /* [5] ACK only (Table 3-35): reflected ECN mark */

/*
 * PDS Control Packet header (Section 3.5.10.8 / 3.5.16, Table 3-38).
 *
 * Wire layout (16 bytes) deliberately mirrors pds_req_hdr / pds_ack_hdr for
 * the first 12 bytes so a CP can ride on the same forward-direction PSN
 * sequence as a request and reuse the existing retransmit / cumulative-ACK
 * machinery: it consumes a PSN and is acknowledged via a normal PDS ACK whose
 * cack_psn covers the CP's psn.
 *
 * The CP subtype is carried in the prologue's next_hdr nibble (bits [10:7]),
 * accessed via pds_ctrl_get_ctl_type / pds_ctrl_set_ctl_type. The trailing
 * 32-bit payload is CP-type-specific (Section 3.5.16.8); for Close Command
 * it is zero.
 */
struct __attribute__((packed)) pds_ctrl_hdr {
    union pds_prologue prologue; /* type = PDS_CP, next_hdr = ctl_type */
    uint16_t probe_opaque;       /* same as ACK (Table 3-38) */
    uint32_t psn;                /* CP consumes a forward PSN */
    uint16_t spdcid;             /* source PDCID */
    uint16_t dpdcid;             /* destination PDCID (or {pdc_info,psn_offset}) */
    uint32_t payload;            /* CP-type-specific (Section 3.5.16.8) */
};

/* PDS Control Packet subtypes -- pds.ctl_type field (Section 3.5.10.8, Table 3-38).
 * Only the values actually implemented today are listed; remaining encodings
 * (Probe=6, Credit=7, Credit Request=8, Negotiation=9) are reserved. */
enum pds_ctl_type {
	PDS_CTL_NOOP = 0,
	PDS_CTL_ACK_REQ = 1,
	PDS_CTL_CLEAR_CMD = 2,
	PDS_CTL_CLEAR_REQ = 3,
	PDS_CTL_CLOSE_CMD = 4, /* initiator-driven PDC close */
	PDS_CTL_CLOSE_REQ = 5, /* target-initiated close request */
};

static inline uint8_t pds_ctrl_get_type(const struct pds_ctrl_hdr *h)
{
	return (ntohs(h->prologue.raw) >> 11) & 0x1F;
}

static inline void pds_ctrl_set_type(struct pds_ctrl_hdr *h, uint8_t type)
{
	uint16_t v = ntohs(h->prologue.raw);
	v = (v & ~(0x1F << 11)) | ((uint16_t) (type & 0x1F) << 11);
	h->prologue.raw = htons(v);
}

static inline uint8_t pds_ctrl_get_flags(const struct pds_ctrl_hdr *h)
{
	return ntohs(h->prologue.raw) & 0x7F;
}

static inline void pds_ctrl_set_flags(struct pds_ctrl_hdr *h, uint8_t flags)
{
	uint16_t v = ntohs(h->prologue.raw);
	v = (v & ~0x7F) | (flags & 0x7F);
	h->prologue.raw = htons(v);
}

static inline uint32_t pds_ctrl_get_psn(const struct pds_ctrl_hdr *h)
{
	return ntohl(h->psn);
}

static inline void pds_ctrl_set_psn(struct pds_ctrl_hdr *h, uint32_t v)
{
	h->psn = htonl(v);
}

static inline uint16_t pds_ctrl_get_spdcid(const struct pds_ctrl_hdr *h)
{
	return ntohs(h->spdcid);
}

static inline void pds_ctrl_set_spdcid(struct pds_ctrl_hdr *h, uint16_t v)
{
	h->spdcid = htons(v);
}

static inline uint16_t pds_ctrl_get_dpdcid(const struct pds_ctrl_hdr *h)
{
	return ntohs(h->dpdcid);
}

static inline void pds_ctrl_set_dpdcid(struct pds_ctrl_hdr *h, uint16_t v)
{
	h->dpdcid = htons(v);
}

/* ctl_type lives in the prologue's next_hdr nibble (bits [10:7]) when
 * pds.type == PDS_CP (Section 3.5.10.2 / 3.5.10.8). */
static inline uint8_t pds_ctrl_get_ctl_type(const struct pds_ctrl_hdr *h)
{
	return (ntohs(h->prologue.raw) >> 7) & 0x0F;
}

static inline void pds_ctrl_set_ctl_type(struct pds_ctrl_hdr *h, uint8_t v)
{
	uint16_t p = ntohs(h->prologue.raw);
	p = (p & ~(0x0F << 7)) | ((uint16_t) (v & 0x0F) << 7);
	h->prologue.raw = htons(p);
}

/* Generic PDS Control Packet header filler (Section 3.5.10.8). */
static inline void pds_ctrl_init(struct pds_ctrl_hdr *h, uint8_t ctl_type,
				 uint32_t psn, uint16_t spdcid, uint16_t dpdcid,
				 uint32_t payload)
{
	memset(h, 0, sizeof(*h));
	pds_ctrl_set_type(h, PDS_CP);
	pds_ctrl_set_ctl_type(h, ctl_type);
	pds_ctrl_set_flags(h, PDS_FLAG_ISROD);
	pds_ctrl_set_psn(h, psn);
	pds_ctrl_set_spdcid(h, spdcid);
	pds_ctrl_set_dpdcid(h, dpdcid);
	h->payload = htonl(payload);
}

static inline uint8_t pds_prologue_get_type(const struct pds_req_hdr *h)
{
    return (ntohs(h->prologue.raw) >> 11) & 0x1F;
}

static inline void pds_prologue_set_type(struct pds_req_hdr *h, uint8_t type)
{
    uint16_t v = ntohs(h->prologue.raw);
    v = (v & ~(0x1F << 11)) | ((uint16_t)(type & 0x1F) << 11);
    h->prologue.raw = htons(v);
}

/* SES next header types -- pds.next_hdr (Table 3-16) */
enum ses_next_hdr {
    UET_HDR_NONE = 0x0,
    UET_HDR_REQUEST_SMALL = 0x1,
    UET_HDR_REQUEST_MEDIUM = 0x2,
    UET_HDR_REQUEST_STD = 0x3,
    UET_HDR_RESPONSE = 0x4,
    UET_HDR_RESPONSE_DATA = 0x5,
    UET_HDR_RESPONSE_DATA_SMALL = 0x6,
};

static inline uint8_t pds_prologue_get_next_hdr(const struct pds_req_hdr *h)
{
    return (ntohs(h->prologue.raw) >> 7) & 0x0F;
}

static inline void pds_prologue_set_next_hdr(struct pds_req_hdr *h, uint8_t nh)
{
    uint16_t v = ntohs(h->prologue.raw);
    v = (v & ~(0x0F << 7)) | ((uint16_t)(nh & 0x0F) << 7);
    h->prologue.raw = htons(v);
}

static inline uint8_t pds_prologue_get_flags(const struct pds_req_hdr *h)
{
    return ntohs(h->prologue.raw) & 0x7F;
}

static inline void pds_prologue_set_flags(struct pds_req_hdr *h, uint8_t flags)
{
    uint16_t v = ntohs(h->prologue.raw);
    v = (v & ~0x7F) | (flags & 0x7F);
    h->prologue.raw = htons(v);
}

/* Spec 3.5.11.4.4: pds.clear_psn_offset is a 16-bit signed two's-complement
 * value, sign-extended to 32 bits and added to pds.psn to obtain CLEAR_PSN.
 * Since CLEAR_PSN <= PSN, the offset is never positive. The accessor returns
 * int16_t so the caller can sign-extend correctly. */
static inline int16_t pds_req_get_clear_psn_offset(const struct pds_req_hdr *h)
{
	return (int16_t) ntohs(h->clear_psn_offset);
}

static inline void pds_req_set_clear_psn_offset(struct pds_req_hdr *h,
						int16_t v)
{
	h->clear_psn_offset = htons((uint16_t) v);
}

static inline uint32_t pds_req_get_psn(const struct pds_req_hdr *h)
{
    return ntohl(h->psn);
}

static inline void pds_req_set_psn(struct pds_req_hdr *h, uint32_t v)
{
    h->psn = htonl(v);
}

/* Convenience: read absolute CLEAR_PSN from a request header. Mirrors
 * pds_ack_get_ack_psn. Sign-extends the 16-bit offset before adding. */
static inline uint32_t pds_req_get_clear_psn(const struct pds_req_hdr *h)
{
	return pds_req_get_psn(h) +
	       (uint32_t) (int32_t) pds_req_get_clear_psn_offset(h);
}

/* Convenience: encode absolute target CLEAR_PSN into the offset field.
 * pds.psn must be set first (this reads it back to compute the offset).
 * Spec 3.5.11.4.4 requires CLEAR_PSN <= PSN, so the encoded offset is
 * never positive. */
static inline void pds_req_set_clear_psn(struct pds_req_hdr *h,
					 uint32_t clear_psn)
{
	pds_req_set_clear_psn_offset(h,
		(int16_t) (int32_t) (clear_psn - pds_req_get_psn(h)));
}

static inline uint16_t pds_req_get_spdcid(const struct pds_req_hdr *h)
{
    return ntohs(h->spdcid);
}

static inline void pds_req_set_spdcid(struct pds_req_hdr *h, uint16_t v)
{
    h->spdcid = htons(v);
}

static inline uint16_t pds_req_get_dpdcid(const struct pds_req_hdr *h)
{
    return ntohs(h->dpdcid.raw);
}

static inline void pds_req_set_dpdcid(struct pds_req_hdr *h, uint16_t v)
{
    h->dpdcid.raw = htons(v);
}

/* SYN-mode dpdcid overlay (flags.syn=1, Table 3-33) */
static inline uint16_t pds_req_get_syn_psn_offset(const struct pds_req_hdr *h)
{
	return ntohs(h->dpdcid.raw) & 0x0FFF;
}

static inline uint8_t pds_req_get_syn_use_rsv_pdc(const struct pds_req_hdr *h)
{
	return (ntohs(h->dpdcid.raw) >> 15) & 1;
}

static inline void pds_req_set_syn_dpdcid(struct pds_req_hdr *h,
				      uint16_t syn_psn_offset,
				      uint8_t use_rsv_pdc)
{
	h->dpdcid.raw = htons((uint16_t) ((use_rsv_pdc & 1) << 15) |
			      (syn_psn_offset & 0x0FFF));
}

/* Convenience: derive Start_PSN from a SYN packet
 * (uet-pdc-architecture.md: Start_PSN = pds.psn - syn_psn_offset).
 * Caller must have already verified pds_prologue_get_flags(h) has SYN set. */
static inline uint32_t pds_req_get_syn_start_psn(const struct pds_req_hdr *h)
{
	return pds_req_get_psn(h) - pds_req_get_syn_psn_offset(h);
}

/* Convenience: encode the 12-bit unsigned SYN psn offset and use_rsv_pdc
 * flag, given the absolute start_psn. pds.psn must be set first. */
static inline void pds_req_set_syn_psn(struct pds_req_hdr *h,
				       uint32_t start_psn,
				       uint8_t use_rsv_pdc)
{
	pds_req_set_syn_dpdcid(h,
		(uint16_t) ((pds_req_get_psn(h) - start_psn) & 0x0FFF),
		use_rsv_pdc);
}

/* true when a < b with 2^31 wraparound (Section 3.5.12) */
static inline int pds_psn_before(uint32_t a, uint32_t b)
{
	return (int32_t) (a - b) < 0;
}

/* true when a >= b with 2^31 wraparound */
static inline int pds_psn_after_eq(uint32_t a, uint32_t b)
{
	return (int32_t) (a - b) >= 0;
}

static inline uint32_t pds_psn_distance(uint32_t a, uint32_t b)
{
	uint32_t fwd = a - b;
	uint32_t bwd = b - a;
	return fwd < bwd ? fwd : bwd;
}

/* Random Start_PSN per Section 3.5.8.2. Pass last_psn=0 for new PDC. */
static inline uint32_t pds_generate_start_psn(uint32_t *seed, uint32_t last_psn)
{
	uint32_t psn;
	do {
		psn = ofi_xorshift_random_r(seed);
	} while (psn == 0 ||
		 pds_psn_distance(psn, last_psn) < PDS_MIN_PSN_DISTANCE);
	return psn;
}

static inline uint8_t pds_ack_get_type(const struct pds_ack_hdr *h)
{
    return (ntohs(h->prologue.raw) >> 11) & 0x1F;
}

static inline void pds_ack_set_type(struct pds_ack_hdr *h, uint8_t type)
{
    uint16_t v = ntohs(h->prologue.raw);
    v = (v & ~(0x1F << 11)) | ((uint16_t)(type & 0x1F) << 11);
    h->prologue.raw = htons(v);
}

static inline uint8_t pds_ack_get_next_hdr(const struct pds_ack_hdr *h)
{
    return (ntohs(h->prologue.raw) >> 7) & 0x0F;
}

static inline void pds_ack_set_next_hdr(struct pds_ack_hdr *h, uint8_t nh)
{
    uint16_t v = ntohs(h->prologue.raw);
    v = (v & ~(0x0F << 7)) | ((uint16_t)(nh & 0x0F) << 7);
    h->prologue.raw = htons(v);
}

static inline uint32_t pds_ack_get_cack_psn(const struct pds_ack_hdr *h)
{
    return ntohl(h->cack_psn);
}

static inline void pds_ack_set_cack_psn(struct pds_ack_hdr *h, uint32_t v)
{
    h->cack_psn = htonl(v);
}

static inline int16_t pds_ack_get_ack_psn_offset(const struct pds_ack_hdr *h)
{
    return (int16_t) ntohs(h->ack_psn_offset);
}

static inline void pds_ack_set_ack_psn_offset(struct pds_ack_hdr *h, int16_t v)
{
    h->ack_psn_offset = htons((uint16_t) v);
}

/* pds.flags.req: 2-bit field in the ACK prologue flags subfield
 * (Section 3.5.11.8.6, Table 3-44). Indicates a target-side request to the
 * initiator. Initiator MUST set to NO_REQUEST on transmit. */
enum pds_ack_req {
	PDS_ACK_REQ_NONE = 0, /* NO_REQUEST */
	PDS_ACK_REQ_CLEAR = 1, /* REQ_CLEAR */
	PDS_ACK_REQ_CLOSE = 2, /* REQ_CLOSE */
};

static inline uint8_t pds_ack_get_req(const struct pds_ack_hdr *h)
{
	return (uint8_t) ((ntohs(h->prologue.raw) >> 1) & 0x3);
}

static inline void pds_ack_set_req(struct pds_ack_hdr *h, uint8_t req)
{
	uint16_t v = ntohs(h->prologue.raw);
	v = (v & ~(0x3u << 1)) | ((uint16_t) (req & 0x3u) << 1);
	h->prologue.raw = htons(v);
}

/* Convenience: derive ACK_PSN from a received ACK header. */
static inline uint32_t pds_ack_get_ack_psn(const struct pds_ack_hdr *h)
{
    return pds_ack_get_cack_psn(h) +
           (uint32_t) (int32_t) pds_ack_get_ack_psn_offset(h);
}

/* Convenience: encode absolute target ACK_PSN into the offset field.
 * cack_psn must be set first (this reads it back to compute the offset).
 * Symmetric inverse of pds_ack_get_ack_psn. */
static inline void pds_ack_set_ack_psn(struct pds_ack_hdr *h,
				       uint32_t ack_psn)
{
	pds_ack_set_ack_psn_offset(h,
		(int16_t) (int32_t) (ack_psn - pds_ack_get_cack_psn(h)));
}

static inline uint16_t pds_ack_get_spdcid(const struct pds_ack_hdr *h)
{
    return ntohs(h->spdcid);
}

static inline void pds_ack_set_spdcid(struct pds_ack_hdr *h, uint16_t v)
{
    h->spdcid = htons(v);
}

static inline uint16_t pds_ack_get_dpdcid(const struct pds_ack_hdr *h)
{
    return ntohs(h->dpdcid);
}

static inline void pds_ack_set_dpdcid(struct pds_ack_hdr *h, uint16_t v)
{
    h->dpdcid = htons(v);
}

/*
 * PDS NACK header (Section 3.5.10, Table 3-32).
 * 16 bytes, matching the spec's uet_pds_nack layout.
 * Carries PDS-only negative acknowledgment metadata.
 */
struct __attribute__((packed)) pds_nack_hdr {
	union pds_prologue prologue;
	uint8_t nack_code; /* NACK reason code */
	uint8_t vendor_code; /* vendor-specific code */
	uint32_t nack_psn; /* PSN that triggered the NACK (big-endian) */
	uint16_t spdcid; /* source PDCID */
	uint16_t dpdcid; /* destination PDCID */
	uint32_t payload; /* context-specific payload */
};

/* NACK reason codes (Section 3.5.10.2) */
enum pds_nack_code {
	PDS_NACK_CODE_TRIMMED = 0x01,
	PDS_NACK_CODE_TRIMMED_LASTHOP = 0x02,
	PDS_NACK_CODE_NO_PKT_BUFFER = 0x07,
	PDS_NACK_CODE_NO_GTD_DEL_AVAIL = 0x08,
	PDS_NACK_CODE_NO_RESOURCE = 0x0a,
	PDS_NACK_CODE_PSN_OOR_WINDOW = 0x0b,
	PDS_NACK_CODE_ROD_OOO = 0x0d, /* OOO on ROD/RUD PDC */
	PDS_NACK_CODE_INV_DPDCID = 0x0e,
	PDS_NACK_CODE_CLOSING = 0x10,
	PDS_NACK_CODE_RCVR_INFER_LOSS = 0x1a,
};

/* NACK prologue: same bit layout as ACK/REQ (type in [15:11], next_hdr [10:7],
 * flags [6:0]). nack_type is flags bit [3] (0=RUD_ROD, 1=RUDI). */
#define PDS_NACK_FLAG_NACK_TYPE (1 << 3)

static inline uint8_t pds_nack_get_type(const struct pds_nack_hdr *h)
{
	return (ntohs(h->prologue.raw) >> 11) & 0x1F;
}

static inline void pds_nack_set_type(struct pds_nack_hdr *h, uint8_t type)
{
	uint16_t v = ntohs(h->prologue.raw);
	v = (v & ~(0x1F << 11)) | ((uint16_t) (type & 0x1F) << 11);
	h->prologue.raw = htons(v);
}

static inline uint8_t pds_nack_get_flags(const struct pds_nack_hdr *h)
{
	return ntohs(h->prologue.raw) & 0x7F;
}

static inline void pds_nack_set_flags(struct pds_nack_hdr *h, uint8_t flags)
{
	uint16_t v = ntohs(h->prologue.raw);
	v = (v & ~0x7F) | (flags & 0x7F);
	h->prologue.raw = htons(v);
}

static inline uint8_t pds_nack_get_code(const struct pds_nack_hdr *h)
{
	return h->nack_code;
}

static inline void pds_nack_set_code(struct pds_nack_hdr *h, uint8_t code)
{
	h->nack_code = code;
}

static inline uint32_t pds_nack_get_psn(const struct pds_nack_hdr *h)
{
	return ntohl(h->nack_psn);
}

static inline void pds_nack_set_psn(struct pds_nack_hdr *h, uint32_t v)
{
	h->nack_psn = htonl(v);
}

static inline uint16_t pds_nack_get_spdcid(const struct pds_nack_hdr *h)
{
	return ntohs(h->spdcid);
}

static inline void pds_nack_set_spdcid(struct pds_nack_hdr *h, uint16_t v)
{
	h->spdcid = htons(v);
}

static inline uint16_t pds_nack_get_dpdcid(const struct pds_nack_hdr *h)
{
	return ntohs(h->dpdcid);
}

static inline void pds_nack_set_dpdcid(struct pds_nack_hdr *h, uint16_t v)
{
	h->dpdcid = htons(v);
}

static inline uint32_t pds_nack_get_payload(const struct pds_nack_hdr *h)
{
	return ntohl(h->payload);
}

static inline void pds_nack_set_payload(struct pds_nack_hdr *h, uint32_t v)
{
	h->payload = htonl(v);
}

static inline void pds_nack_init(struct pds_nack_hdr *h, uint8_t nack_code,
				 uint32_t nack_psn, uint16_t spdcid,
				 uint16_t dpdcid)
{
	memset(h, 0, sizeof(*h));
	pds_nack_set_type(h, PDS_NACK);
	pds_nack_set_code(h, nack_code);
	pds_nack_set_psn(h, nack_psn);
	pds_nack_set_spdcid(h, spdcid);
	pds_nack_set_dpdcid(h, dpdcid);
}

/* 
 *
 * SES (Semantic Sublayer) Protocol Definitions (Section 3.4)
 *
 */

/* SES protocol version (ses.ver = 0 for initial version) */
#define SES_VERSION 0

/* SES header_data (64 bits). */
union ses_header_data {
    uint64_t raw;
    /* DO NOT ACCESS -- Union members below are solely for the documentation purpose. */
    uint64_t completion_data;         /* som=1, hd=1: completion data (Table 3-8) */
    struct {                          /* som=0: continuation fields (Table 3-9) */
        uint64_t message_offset : 32; /* [31:0]  byte offset in message */
        uint64_t payload_length : 14; /* [45:32] payload bytes in pkt */
        uint64_t _reserved      : 18; /* [63:46] must be 0 */
    } cont;
};

/* SES Standard Header (Tables 3-8/3-9). */
struct __attribute__((packed)) ses_req_hdr {
    uint16_t ctrl_flags;     /* opcode, ver, dc, ie, rel, hd, eom, som */
    uint16_t message_id;     /* 16-bit message identifier */
    uint8_t  ri_generation;
    uint8_t  job_id[3];      /* 24-bit JobID */
    uint16_t pid_on_fep;     /* [15:12] rsvd, [11:0] PIDonFEP */
    uint16_t resource_index; /* [15:12] rsvd, [11:0] resource_index */
    uint64_t buffer_offset;  /* offset within target buffer */
    uint32_t initiator;      /* initiator ID (rank) */
    uint64_t match_bits;     /* tag match bits / memory key */
    union ses_header_data header_data;
    uint32_t request_length; /* total transfer length */
};

#define SUET_PID_ON_FEP_MASK 0x0FFF  /* 12-bit significant */
#define SUET_RI_MASK         0x0FFF  /* 12-bit significant */

/* SES Opcodes (Table 3-17) */
enum ses_opcode {
    UET_NO_OP = 0x00,
    UET_WRITE = 0x01,
    UET_READ = 0x02,
    UET_ATOMIC = 0x03,
    UET_FETCHING_ATOMIC = 0x04,
    UET_SEND = 0x05,
    UET_RENDEZVOUS_SEND = 0x06,
    UET_DATAGRAM_SEND = 0x07,
    UET_DEFERRABLE_SEND = 0x08,
    UET_TAGGED_SEND = 0x09,
    UET_RENDEZVOUS_TSEND = 0x0A,
    UET_DEFERRABLE_TSEND = 0x0B,
    UET_DEFERRABLE_RTR = 0x0C,
    UET_TSEND_ATOMIC = 0x0D,
    UET_TSEND_FETCH_ATOMIC = 0x0E,
    UET_MSG_ERROR = 0x0F,
};

/* SES Response Opcodes (Table 3-18) */
enum ses_resp_opcode {
    UET_DEFAULT_RESPONSE = 0x00,
    UET_RESPONSE = 0x01,
    UET_RESPONSE_W_DATA = 0x02,
    UET_NO_RESPONSE = 0x03,
};

/* SES Return Codes (Table 3-19) */
enum ses_return_code {
    RC_NULL = 0x00,
    RC_OK = 0x01,
    RC_BAD_GENERATION = 0x02,
    RC_DISABLED = 0x03,
    RC_DISABLED_GEN = 0x04,
    RC_NO_MATCH = 0x05,
    RC_UNSUPPORTED_OP = 0x06,
    RC_UNSUPPORTED_SIZE = 0x07,
    RC_AT_INVALID = 0x08,
    RC_AT_PERM = 0x09,
    RC_AT_ATS_ERROR = 0x0A,
    RC_AT_NO_TRANS = 0x0B,
    RC_AT_OUT_OF_RANGE = 0x0C,
    RC_HOST_POISONED = 0x0D,
    RC_HOST_UNSUCCESS_CMPL = 0x0E,
    RC_AMO_UNSUPPORTED_OP = 0x0F,
    RC_AMO_UNSUPPORTED_DT = 0x10,
    RC_AMO_UNSUPPORTED_SIZE = 0x11,
    RC_AMO_UNALIGNED = 0x12,
    RC_AMO_FP_NAN = 0x13,
    RC_AMO_FP_UNDERFLOW = 0x14,
    RC_AMO_FP_OVERFLOW = 0x15,
    RC_AMO_FP_INEXACT = 0x16,
    RC_PERM_VIOLATION = 0x17,
    RC_OP_VIOLATION = 0x18,
    RC_BAD_INDEX = 0x19,
    RC_BAD_PID = 0x1A,
    RC_BAD_JOB_ID = 0x1B,
    RC_BAD_MKEY = 0x1C,
    RC_BAD_ADDR = 0x1D,
    RC_CANCELLED = 0x1E,
    RC_UNDELIVERABLE = 0x1F,
    RC_UNCOR = 0x20,
    RC_UNCOR_TRNSNT = 0x21,
    RC_TOO_LONG = 0x22,
    RC_INITIATOR_ERROR = 0x23,
    RC_DROPPED = 0x24,
};

/* SES List where message was delivered (Table 3-20) */
enum ses_list_type {
    UET_EXPECTED = 0x0,
    UET_OVERFLOW = 0x1,
};

/* SES req ctrl_flags layout (Table 3-8, 16 bits) */
#define SES_REQ_CTRL_OPCODE_SHIFT 8        /* [13:8]  opcode (6 bits) */
#define SES_REQ_CTRL_OPCODE_MASK  0x3F00
#define SES_REQ_CTRL_VER_SHIFT    6        /* [7:6]   version (2 bits) */
#define SES_REQ_CTRL_VER_MASK     0x00C0
#define SES_REQ_CTRL_DC           (1 << 5) /* [5] delivery complete */
#define SES_REQ_CTRL_IE           (1 << 4) /* [4] initiator error */
#define SES_REQ_CTRL_REL          (1 << 3) /* [3] relative addressing */
#define SES_REQ_CTRL_HD      	  (1 << 2) /* [2] header data present */
#define SES_REQ_CTRL_EOM      	  (1 << 1) /* [1] end of message */
#define SES_REQ_CTRL_SOM      	  (1 << 0) /* [0] start of message */

static inline void ses_req_ctrl_init(struct ses_req_hdr *hdr, uint8_t opcode,
				     int som, int eom, int rel, int hd)
{
	uint16_t ctrl = (uint16_t) (opcode & 0x3F) << SES_REQ_CTRL_OPCODE_SHIFT;
	ctrl |= (SES_VERSION & 0x3) << SES_REQ_CTRL_VER_SHIFT;
	if (som)
		ctrl |= SES_REQ_CTRL_SOM;
	if (eom)
		ctrl |= SES_REQ_CTRL_EOM;
	if (rel)
		ctrl |= SES_REQ_CTRL_REL;
	if (hd)
		ctrl |= SES_REQ_CTRL_HD;
	hdr->ctrl_flags = htons(ctrl);
}

static inline uint8_t ses_req_ctrl_get_opcode(const struct ses_req_hdr *hdr)
{
	return (ntohs(hdr->ctrl_flags) & SES_REQ_CTRL_OPCODE_MASK) >>
	       SES_REQ_CTRL_OPCODE_SHIFT;
}

static inline int ses_req_ctrl_is_som(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->ctrl_flags) & SES_REQ_CTRL_SOM;
}

static inline int ses_req_ctrl_is_eom(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->ctrl_flags) & SES_REQ_CTRL_EOM;
}

static inline int ses_req_ctrl_has_hd(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->ctrl_flags) & SES_REQ_CTRL_HD;
}

static inline void ses_req_ctrl_set_som_eom(struct ses_req_hdr *hdr, bool som,
					    bool eom)
{
	uint16_t c = ntohs(hdr->ctrl_flags);
	c = (c & ~(SES_REQ_CTRL_SOM | SES_REQ_CTRL_EOM)) |
	    (som ? SES_REQ_CTRL_SOM : 0) | (eom ? SES_REQ_CTRL_EOM : 0);
	hdr->ctrl_flags = htons(c);
}

static inline uint16_t ses_get_message_id(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->message_id);
}

static inline void ses_set_message_id(struct ses_req_hdr *hdr, uint16_t v)
{
	hdr->message_id = htons(v);
}

static inline uint16_t ses_get_ri(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->resource_index) & SUET_RI_MASK;
}

static inline void ses_set_ri(struct ses_req_hdr *hdr, uint16_t v)
{
	hdr->resource_index = htons(v & SUET_RI_MASK);
}

static inline uint16_t ses_get_pid_on_fep(const struct ses_req_hdr *hdr)
{
	return ntohs(hdr->pid_on_fep) & SUET_PID_ON_FEP_MASK;
}

static inline void ses_set_pid_on_fep(struct ses_req_hdr *hdr, uint16_t v)
{
	hdr->pid_on_fep = htons(v & SUET_PID_ON_FEP_MASK);
}

static inline uint64_t ses_get_buffer_offset(const struct ses_req_hdr *hdr)
{
	return ntohll(hdr->buffer_offset);
}
static inline void ses_set_buffer_offset(struct ses_req_hdr *hdr, uint64_t v)
{
	hdr->buffer_offset = htonll(v);
}

static inline uint32_t ses_get_initiator(const struct ses_req_hdr *hdr)
{
	return ntohl(hdr->initiator);
}

static inline void ses_set_initiator(struct ses_req_hdr *hdr, uint32_t v)
{
	hdr->initiator = htonl(v);
}

static inline uint64_t ses_get_match_bits(const struct ses_req_hdr *hdr)
{
	return ntohll(hdr->match_bits);
}
static inline void ses_set_match_bits(struct ses_req_hdr *hdr, uint64_t v)
{
	hdr->match_bits = htonll(v);
}

static inline uint32_t ses_get_request_length(const struct ses_req_hdr *hdr)
{
	return ntohl(hdr->request_length);
}

static inline void ses_set_request_length(struct ses_req_hdr *hdr, uint32_t v)
{
	hdr->request_length = htonl(v);
}

static inline uint64_t
ses_hd_get_completion_data(const struct ses_req_hdr *hdr)
{
	return ntohll(hdr->header_data.raw);
}

static inline void ses_hd_set_completion_data(struct ses_req_hdr *hdr,
					      uint64_t v)
{
	hdr->header_data.raw = htonll(v);
}

static inline uint32_t
ses_hd_get_message_offset(const struct ses_req_hdr *hdr)
{
	return (uint32_t) ntohll(hdr->header_data.raw);
}

static inline uint16_t
ses_hd_get_payload_length(const struct ses_req_hdr *hdr)
{
	return (uint16_t) ((ntohll(hdr->header_data.raw) >> 32) & 0x3FFF);
}

static inline void ses_hd_set_cont(struct ses_req_hdr *hdr,
				   uint32_t msg_offset, uint16_t payload_len)
{
	uint64_t v = ((uint64_t) (payload_len & 0x3FFF) << 32) |
		     (uint64_t) msg_offset;
	hdr->header_data.raw = htonll(v);
}

static inline void ses_req_init(struct ses_req_hdr *hdr, uint8_t opcode,
				int is_som, int is_eom, int rel, int hd,
				uint16_t message_id, uint64_t buffer_offset,
				uint64_t match_bits, uint64_t completion_data,
				uint32_t request_length, uint16_t pid_on_fep,
				uint16_t resource_index)
{
	ses_req_ctrl_init(hdr, opcode, is_som, is_eom, rel, hd);
	ses_set_message_id(hdr, message_id);
	hdr->ri_generation = 0;
	memset(hdr->job_id, 0, sizeof(hdr->job_id));
	ses_set_pid_on_fep(hdr, pid_on_fep);
	ses_set_ri(hdr, resource_index);
	ses_set_buffer_offset(hdr, buffer_offset);
	ses_set_initiator(hdr, 0);
	ses_set_match_bits(hdr, match_bits);

	if (is_som)
		ses_hd_set_completion_data(hdr, completion_data);
	else
		hdr->header_data.raw = 0;

	ses_set_request_length(hdr, request_length);
}

/* SES Response Header (Table 3-11). Network byte order. Use ses_resp_get/ses_resp_set. */
struct __attribute__((packed)) ses_resp_hdr {
    uint16_t ctrl_flags;      /* [15:14] list, [13:8] opcode, [7:6] ver, [5:0] return_code */
    uint16_t message_id;      /* message ID of original request */
    uint8_t  ri_generation;   /* single byte -- no swap */
    uint8_t  job_id[3];	      /* 24-bit JobID -- byte array, no swap */
    uint32_t modified_length; /* bytes modified */
};

/* Response ctrl_flags accessors */
#define SES_RESP_CTRL_LIST_SHIFT   14
#define SES_RESP_CTRL_LIST_MASK    0xC000
#define SES_RESP_CTRL_OPCODE_SHIFT 8
#define SES_RESP_CTRL_OPCODE_MASK  0x3F00
#define SES_RESP_CTRL_VER_SHIFT    6
#define SES_RESP_CTRL_VER_MASK     0x00C0
#define SES_RESP_CTRL_RC_MASK      0x003F

static inline uint8_t ses_resp_ctrl_get_list(const struct ses_resp_hdr *hdr)
{
	return (ntohs(hdr->ctrl_flags) & SES_RESP_CTRL_LIST_MASK) >>
	       SES_RESP_CTRL_LIST_SHIFT;
}

static inline uint8_t ses_resp_ctrl_get_opcode(const struct ses_resp_hdr *hdr)
{
	return (ntohs(hdr->ctrl_flags) & SES_RESP_CTRL_OPCODE_MASK) >>
	       SES_RESP_CTRL_OPCODE_SHIFT;
}

static inline uint8_t ses_resp_ctrl_get_rc(const struct ses_resp_hdr *hdr)
{
	return ntohs(hdr->ctrl_flags) & SES_RESP_CTRL_RC_MASK;
}

static inline void ses_resp_ctrl_init(struct ses_resp_hdr *hdr, uint8_t list,
				      uint8_t opcode, uint8_t ver, uint8_t rc)
{
	hdr->ctrl_flags = htons(
		((uint16_t) (list & 0x3) << SES_RESP_CTRL_LIST_SHIFT) |
		((uint16_t) (opcode & 0x3F) << SES_RESP_CTRL_OPCODE_SHIFT) |
		((uint16_t) (ver & 0x3) << SES_RESP_CTRL_VER_SHIFT) |
		(uint16_t) (rc & 0x3F));
}

static inline uint16_t ses_resp_get_message_id(const struct ses_resp_hdr *hdr)
{
	return ntohs(hdr->message_id);
}

static inline void ses_resp_set_message_id(struct ses_resp_hdr *hdr, uint16_t v)
{
	hdr->message_id = htons(v);
}

static inline uint32_t
ses_resp_get_modified_length(const struct ses_resp_hdr *hdr)
{
	return ntohl(hdr->modified_length);
}

static inline void ses_resp_set_modified_length(struct ses_resp_hdr *hdr,
						uint32_t v)
{
	hdr->modified_length = htonl(v);
}

static inline void ses_resp_init(struct ses_resp_hdr *hdr, uint8_t list,
                 uint8_t opcode, uint8_t rc,
                 uint16_t message_id, uint32_t modified_length)
{
    ses_resp_ctrl_init(hdr, list, opcode, SES_VERSION, rc);
    ses_resp_set_message_id(hdr, message_id);
    hdr->ri_generation = 0;
    memset(hdr->job_id, 0, sizeof(hdr->job_id));
    ses_resp_set_modified_length(hdr, modified_length);
}

/* SES AMO Operation Opcodes (Table 3-21) */
enum ses_amo_opcode {
    UET_AMO_MIN = 0x00,
    UET_AMO_MAX = 0x01,
    UET_AMO_SUM = 0x02,
    UET_AMO_DIFF = 0x03,
    UET_AMO_PROD = 0x04,
    UET_AMO_LOR = 0x05,
    UET_AMO_LAND = 0x06,
    UET_AMO_BOR = 0x07,
    UET_AMO_BAND = 0x08,
    UET_AMO_LXOR = 0x09,
    UET_AMO_BXOR = 0x0A,
    UET_AMO_READ = 0x0B,
    UET_AMO_WRITE = 0x0C,
    UET_AMO_CSWAP = 0x0D,
    UET_AMO_CSWAP_NE = 0x0E,
    UET_AMO_CSWAP_LE = 0x0F,
    UET_AMO_CSWAP_LT = 0x10,
    UET_AMO_CSWAP_GE = 0x11,
    UET_AMO_CSWAP_GT = 0x12,
    UET_AMO_MSWAP = 0x13,
    UET_AMO_INVAL = 0x14,
};

/* SES AMO Datatypes (Table 3-22) */
enum ses_amo_datatype {
    UET_TYPE_INT8 = 0x00,
    UET_TYPE_UINT8 = 0x01,
    UET_TYPE_INT16 = 0x02,
    UET_TYPE_UINT16 = 0x03,
    UET_TYPE_INT32 = 0x04,
    UET_TYPE_UINT32 = 0x05,
    UET_TYPE_INT64 = 0x06,
    UET_TYPE_UINT64 = 0x07,
    UET_TYPE_INT128 = 0x08,
    UET_TYPE_UINT128 = 0x09,
    UET_TYPE_FLOAT = 0x0A,
    UET_TYPE_DOUBLE = 0x0B,
    UET_TYPE_FLOAT_COMPLEX = 0x0C,
    UET_TYPE_DOUBLE_COMPLEX = 0x0D,
    UET_TYPE_LONG_DOUBLE = 0x0E,
    UET_TYPE_LONG_DOUBLE_COMPLEX = 0x0F,
    UET_TYPE_BF16 = 0x10,
    UET_TYPE_FP16 = 0x11,
};

/* SES AMO Semantic Control (Table 3-23) */
#define UET_AMO_CTRL_CACHEABLE      (1 << 0)
#define UET_AMO_CTRL_CPU_COHERENT (1 << 1)

/*
 * Map libfabric fi_op to SES AMO opcode (Table 3-21).
 */
static inline uint8_t fi_op_to_ses_amo(enum fi_op op)
{
    switch (op) {
    case FI_MIN:
        return UET_AMO_MIN;
    case FI_MAX:
        return UET_AMO_MAX;
    case FI_SUM:
        return UET_AMO_SUM;
    case FI_PROD:
        return UET_AMO_PROD;
    case FI_LOR:
        return UET_AMO_LOR;
    case FI_LAND:
        return UET_AMO_LAND;
    case FI_BOR:
        return UET_AMO_BOR;
    case FI_BAND:
        return UET_AMO_BAND;
    case FI_LXOR:
        return UET_AMO_LXOR;
    case FI_BXOR:
        return UET_AMO_BXOR;
    case FI_ATOMIC_READ:
        return UET_AMO_READ;
    case FI_ATOMIC_WRITE:
        return UET_AMO_WRITE;
    case FI_CSWAP:
        return UET_AMO_CSWAP;
    case FI_CSWAP_NE:
        return UET_AMO_CSWAP_NE;
    case FI_CSWAP_LE:
        return UET_AMO_CSWAP_LE;
    case FI_CSWAP_LT:
        return UET_AMO_CSWAP_LT;
    case FI_CSWAP_GE:
        return UET_AMO_CSWAP_GE;
    case FI_CSWAP_GT:
        return UET_AMO_CSWAP_GT;
    case FI_MSWAP:
        return UET_AMO_MSWAP;
    default:
        return UET_AMO_WRITE;
    }
}

/*
 * Map SES AMO opcode back to libfabric fi_op.
 */
static inline enum fi_op ses_amo_to_fi_op(uint8_t amo)
{
    switch (amo) {
    case UET_AMO_MIN:
        return FI_MIN;
    case UET_AMO_MAX:
        return FI_MAX;
    case UET_AMO_SUM:
        return FI_SUM;
    case UET_AMO_DIFF:
        return FI_SUM; /* no FI_DIFF; closest match */
    case UET_AMO_PROD:
        return FI_PROD;
    case UET_AMO_LOR:
        return FI_LOR;
    case UET_AMO_LAND:
        return FI_LAND;
    case UET_AMO_BOR:
        return FI_BOR;
    case UET_AMO_BAND:
        return FI_BAND;
    case UET_AMO_LXOR:
        return FI_LXOR;
    case UET_AMO_BXOR:
        return FI_BXOR;
    case UET_AMO_READ:
        return FI_ATOMIC_READ;
    case UET_AMO_WRITE:
        return FI_ATOMIC_WRITE;
    case UET_AMO_CSWAP:
        return FI_CSWAP;
    case UET_AMO_CSWAP_NE:
        return FI_CSWAP_NE;
    case UET_AMO_CSWAP_LE:
        return FI_CSWAP_LE;
    case UET_AMO_CSWAP_LT:
        return FI_CSWAP_LT;
    case UET_AMO_CSWAP_GE:
        return FI_CSWAP_GE;
    case UET_AMO_CSWAP_GT:
        return FI_CSWAP_GT;
    case UET_AMO_MSWAP:
        return FI_MSWAP;
    default:
        return FI_ATOMIC_WRITE;
    }
}

/*
 * Map libfabric fi_datatype to SES AMO datatype (Table 3-22).
 * The encodings match 1:1 for the common types.
 */
static inline uint8_t fi_datatype_to_ses_type(enum fi_datatype dt)
{
    switch (dt) {
    case FI_INT8:
        return UET_TYPE_INT8;
    case FI_UINT8:
        return UET_TYPE_UINT8;
    case FI_INT16:
        return UET_TYPE_INT16;
    case FI_UINT16:
        return UET_TYPE_UINT16;
    case FI_INT32:
        return UET_TYPE_INT32;
    case FI_UINT32:
        return UET_TYPE_UINT32;
    case FI_INT64:
        return UET_TYPE_INT64;
    case FI_UINT64:
        return UET_TYPE_UINT64;
    case FI_INT128:
        return UET_TYPE_INT128;
    case FI_UINT128:
        return UET_TYPE_UINT128;
    case FI_FLOAT:
        return UET_TYPE_FLOAT;
    case FI_DOUBLE:
        return UET_TYPE_DOUBLE;
    case FI_FLOAT_COMPLEX:
        return UET_TYPE_FLOAT_COMPLEX;
    case FI_DOUBLE_COMPLEX:
        return UET_TYPE_DOUBLE_COMPLEX;
    case FI_LONG_DOUBLE:
        return UET_TYPE_LONG_DOUBLE;
    case FI_LONG_DOUBLE_COMPLEX:
        return UET_TYPE_LONG_DOUBLE_COMPLEX;
    case FI_BFLOAT16:
        return UET_TYPE_BF16;
    case FI_FLOAT16:
        return UET_TYPE_FP16;
    default:
        return UET_TYPE_UINT8;
    }
}

/*
 * Map SES AMO datatype back to libfabric fi_datatype.
 */
static inline enum fi_datatype ses_type_to_fi_datatype(uint8_t st)
{
    switch (st) {
    case UET_TYPE_INT8:
        return FI_INT8;
    case UET_TYPE_UINT8:
        return FI_UINT8;
    case UET_TYPE_INT16:
        return FI_INT16;
    case UET_TYPE_UINT16:
        return FI_UINT16;
    case UET_TYPE_INT32:
        return FI_INT32;
    case UET_TYPE_UINT32:
        return FI_UINT32;
    case UET_TYPE_INT64:
        return FI_INT64;
    case UET_TYPE_UINT64:
        return FI_UINT64;
    case UET_TYPE_INT128:
        return FI_INT128;
    case UET_TYPE_UINT128:
        return FI_UINT128;
    case UET_TYPE_FLOAT:
        return FI_FLOAT;
    case UET_TYPE_DOUBLE:
        return FI_DOUBLE;
    case UET_TYPE_FLOAT_COMPLEX:
        return FI_FLOAT_COMPLEX;
    case UET_TYPE_DOUBLE_COMPLEX:
        return FI_DOUBLE_COMPLEX;
    case UET_TYPE_LONG_DOUBLE:
        return FI_LONG_DOUBLE;
    case UET_TYPE_LONG_DOUBLE_COMPLEX:
        return FI_LONG_DOUBLE_COMPLEX;
    case UET_TYPE_BF16:
        return FI_BFLOAT16;
    case UET_TYPE_FP16:
        return FI_FLOAT16;
    default:
        return FI_VOID;
    }
}

static inline int ses_req_opcode_is_tagged(uint8_t opcode)
{
    return opcode == UET_TAGGED_SEND || opcode == UET_RENDEZVOUS_TSEND ||
           opcode == UET_DEFERRABLE_TSEND || opcode == UET_TSEND_ATOMIC ||
           opcode == UET_TSEND_FETCH_ATOMIC;
}

static inline int ses_req_opcode_is_rma(uint8_t opcode)
{
    /* UET_READ / UET_FETCHING_ATOMIC are spec opcodes but the suet
     * provider doesn't emit them (caps don't advertise FI_READ /
     * fetching atomic; see prov/suet/README.md). */
    return opcode == UET_WRITE || opcode == UET_ATOMIC;
}

static inline int ses_req_opcode_is_write(uint8_t opcode)
{
    return opcode == UET_WRITE;
}

static inline int ses_req_opcode_is_send(uint8_t opcode)
{
    return opcode == UET_SEND || opcode == UET_RENDEZVOUS_SEND ||
           opcode == UET_DEFERRABLE_SEND || opcode == UET_DATAGRAM_SEND;
}

/*
 * Map SES opcode (UET Spec Table 3-17) to internal ofi_op_* value
 * for rx entry initialization.
 */
static inline uint32_t ses_req_opcode_to_ofi_op(uint8_t ses_opcode)
{
    switch (ses_opcode) {
    case UET_SEND:
    case UET_RENDEZVOUS_SEND:
    case UET_DATAGRAM_SEND:
    case UET_DEFERRABLE_SEND:
        return ofi_op_msg;
    case UET_TAGGED_SEND:
    case UET_RENDEZVOUS_TSEND:
    case UET_DEFERRABLE_TSEND:
        return ofi_op_tagged;
    case UET_WRITE:
        return ofi_op_write;
    case UET_ATOMIC:
        return ofi_op_atomic;
    /* UET_READ / UET_FETCHING_ATOMIC: not emitted by this provider. */
    default:
        return ofi_op_max;
    }
}

/*
 * Map a libfabric ofi_op_* value to the corresponding SES wire opcode
 * (UET Spec Table 3-17).
 */
static inline uint8_t ofi_op_to_ses_req_opcode(uint32_t op)
{
    switch (op) {
    case ofi_op_msg:
        return UET_SEND;
    case ofi_op_tagged:
        return UET_TAGGED_SEND;
    case ofi_op_write:
        return UET_WRITE;
    case ofi_op_atomic:
        return UET_ATOMIC;
    /* ofi_op_read_req / atomic_fetch / atomic_compare not emitted by
     * this provider; corresponding ops return -FI_ENOSYS. */
    default:
        return UET_NO_OP;
    }
}

/* Request packet: pds_req_hdr + ses_req_hdr + optional extension hdrs + payload */
struct __attribute__((packed)) suet_req_pkt {
    struct pds_req_hdr pds;
    struct ses_req_hdr ses;
    char msg[];
};

/* ACK packet: pds_ack_hdr + ses_resp_hdr (Table 3-35) */
struct __attribute__((packed)) suet_ack_pkt {
    struct pds_ack_hdr pds;
    struct ses_resp_hdr ses;
};

/* Control packet: pds_ctrl_hdr only (no SES payload). Section 3.5.16. */
struct __attribute__((packed)) suet_ctrl_pkt {
    struct pds_ctrl_hdr pds;
};

/* SES Atomic Extension Header (Figure 3-16, Section 3.4.2.4) */
struct __attribute__((packed)) ses_msg_amo_hdr {
    uint8_t atomic_opcode;     /* UET AMO operation (Table 3-21) */
    uint8_t atomic_datatype;   /* UET AMO datatype (Table 3-22) */
    uint8_t semantic_control;  /* AMO control flags (Table 3-23) */
    uint8_t reserved;
};

/* Atomic request packet: pds + ses + amo_hdr + payload */
struct __attribute__((packed)) suet_amo_pkt {
    struct pds_req_hdr pds;
    struct ses_req_hdr ses;
    struct ses_msg_amo_hdr amo;
    char msg[];
};

#endif
