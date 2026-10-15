/*
 * proto.c - encoding and decoding of minirouted packets.
 *
 * Every packet starts with an 8-byte header:
 *   version(1) type(1) length(2) sender_router_id(4)
 *
 * HELLO body:  nseen(2) pad(2) seen_router_id(4) * nseen
 * LSA body:    origin(4) seq(4) nlinks(2) npfx(2)
 *              { nbr(4) cost(4) } * nlinks
 *              { addr(4) len(1) pad(3) } * npfx
 *
 * Fields are written byte by byte in network order, so the format does not
 * depend on struct padding or host endianness. Decoders check every length
 * before reading, because packets come from the network and can't be trusted.
 */
#include "minirouted.h"

#include <string.h>

#define HDR_LEN   8
#define HELLO_FIX 4
#define LSA_FIX   12

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_header(uint8_t *buf, uint8_t type, size_t len, uint32_t sender)
{
    buf[0] = PROTO_VERSION;
    buf[1] = type;
    put16(buf + 2, (uint16_t)len);
    put32(buf + 4, sender);
}

size_t encode_hello(uint8_t *buf, size_t cap, const struct router *r)
{
    /* List every neighbor we have heard from. When a neighbor sees its own
     * id in our list it knows traffic flows both ways (the 2-way check). */
    int cnt = 0;
    for (int i = 0; i < r->nnbrs; i++)
        if (r->nbrs[i].state != NBR_DOWN)
            cnt++;

    size_t len = HDR_LEN + HELLO_FIX + 4u * (size_t)cnt;
    if (len > cap)
        return 0;

    uint8_t *p = buf + HDR_LEN;
    put16(p, (uint16_t)cnt);
    put16(p + 2, 0);
    p += HELLO_FIX;
    for (int i = 0; i < r->nnbrs; i++) {
        if (r->nbrs[i].state != NBR_DOWN) {
            put32(p, r->nbrs[i].id);
            p += 4;
        }
    }
    put_header(buf, MSG_HELLO, len, r->id);
    return len;
}

size_t encode_lsa(uint8_t *buf, size_t cap, uint32_t sender, const struct lsa *l)
{
    size_t len = HDR_LEN + LSA_FIX + 8u * l->nlinks + 8u * l->npfx;
    if (len > cap)
        return 0;

    uint8_t *p = buf + HDR_LEN;
    put32(p, l->origin);
    put32(p + 4, l->seq);
    put16(p + 8, l->nlinks);
    put16(p + 10, l->npfx);
    p += LSA_FIX;
    for (int i = 0; i < l->nlinks; i++, p += 8) {
        put32(p, l->links[i].nbr);
        put32(p + 4, l->links[i].cost);
    }
    for (int i = 0; i < l->npfx; i++, p += 8) {
        put32(p, l->pfx[i].addr);
        p[4] = l->pfx[i].len;
        p[5] = p[6] = p[7] = 0;
    }
    put_header(buf, MSG_LSA, len, sender);
    return len;
}

int decode_header(const uint8_t *buf, size_t n, uint8_t *type, uint32_t *sender)
{
    if (n < HDR_LEN || buf[0] != PROTO_VERSION)
        return -1;
    if (get16(buf + 2) != n) /* truncated or trailing junk */
        return -1;
    *type = buf[1];
    *sender = get32(buf + 4);
    return *sender == 0 ? -1 : 0;
}

int decode_hello(const uint8_t *buf, size_t n, uint32_t *seen, int max, int *nseen)
{
    if (n < HDR_LEN + HELLO_FIX)
        return -1;
    const uint8_t *p = buf + HDR_LEN;
    uint16_t cnt = get16(p);
    if (n != HDR_LEN + HELLO_FIX + 4u * cnt)
        return -1;
    p += HELLO_FIX;
    *nseen = 0;
    for (int i = 0; i < cnt; i++, p += 4)
        if (*nseen < max)
            seen[(*nseen)++] = get32(p);
    return 0;
}

int decode_lsa(const uint8_t *buf, size_t n, struct lsa *out)
{
    if (n < HDR_LEN + LSA_FIX)
        return -1;
    const uint8_t *p = buf + HDR_LEN;
    memset(out, 0, sizeof *out);
    out->origin = get32(p);
    out->seq = get32(p + 4);
    uint16_t nl = get16(p + 8), np = get16(p + 10);
    if (out->origin == 0 || nl > MAX_NBRS || np > MAX_PFX)
        return -1;
    if (n != HDR_LEN + LSA_FIX + 8u * nl + 8u * np)
        return -1;
    p += LSA_FIX;

    out->nlinks = nl;
    for (int i = 0; i < nl; i++, p += 8) {
        out->links[i].nbr = get32(p);
        out->links[i].cost = get32(p + 4);
        if (out->links[i].cost == 0)
            return -1; /* zero-cost links would break SPF tie-breaking */
    }
    out->npfx = np;
    for (int i = 0; i < np; i++, p += 8) {
        uint8_t len = p[4];
        if (len > 32)
            return -1;
        uint32_t mask = len ? 0xFFFFFFFFu << (32 - len) : 0;
        out->pfx[i].addr = get32(p) & mask;
        out->pfx[i].len = len;
    }
    return 0;
}
