// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// udpnet: see udpnet.h.
#include "udpnet.h"

static void copy(void* d, const void* s, uint32_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; }
static void zero(void* p, uint32_t n) { uint8_t* b = p; while (n--) *b++ = 0; }
static int same(const void* a, const void* b, uint32_t n)
{
    const uint8_t* x = a; const uint8_t* y = b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t ipAt(const uint8_t* p) { uint32_t v; copy(&v, p, 4); return v; }
static const uint8_t kBcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// the ones' complement sum, folded (RFC 1071), over a run of bytes, carried on from sum
static uint32_t sum(uint32_t s, const uint8_t* p, uint32_t n)
{
    for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)(p[0] << 8);
    return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xffff) + (s >> 16); return (uint16_t)~s; }

void un_begin(un* u, const uint8_t mac[6], uint32_t ip, uint32_t mask, un_frame_fn tx, un_udp_fn udp, void* ctx)
{
    zero(u, sizeof *u);
    copy(u->mac, mac, 6);
    u->ip = ip; u->mask = mask; u->tx = tx; u->udp = udp; u->ctx = ctx;
}

uint32_t un_broadcast(const un* u) { return u->ip | ~u->mask; }

static void learn(un* u, uint32_t ip, const uint8_t mac[6], uint32_t now)
{
    if (!ip || ip == 0xffffffffu || (mac[0] & 1)) return;   // (no broadcast or multicast senders)
    int slot = -1;
    for (int i = 0; i < UN_ARP; i++) if (u->arp[i].valid && u->arp[i].ip == ip) { slot = i; break; }
    if (slot < 0) {                               // a free one, else the longest unused
        uint32_t oldest = 0;
        for (int i = 0; i < UN_ARP; i++) {
            if (!u->arp[i].valid) { slot = i; break; }
            if (now - u->arp[i].usedUs >= oldest) { oldest = now - u->arp[i].usedUs; slot = i; }
        }
    }
    u->arp[slot].ip = ip; copy(u->arp[slot].mac, mac, 6); u->arp[slot].valid = 1; u->arp[slot].usedUs = now;
}

static void arp(un* u, uint16_t op, const uint8_t* toMac, uint32_t toIp)
{
    uint8_t* f = u->out;
    zero(f, 60);
    copy(f, op == 1 ? kBcast : toMac, 6);
    copy(f + 6, u->mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1); put16(f + 16, 0x0800); f[18] = 6; f[19] = 4; put16(f + 20, op);
    copy(f + 22, u->mac, 6); copy(f + 28, &u->ip, 4);
    if (op == 2) copy(f + 32, toMac, 6);
    copy(f + 38, &toIp, 4);
    u->tx(u->ctx, f, 60);                         // (the minimum frame, padded)
}

void un_announce(un* u) { arp(u, 1, kBcast, u->ip); }

void un_rx(un* u, const uint8_t* f, uint32_t n, uint32_t now)
{
    if (n < 14 || same(f + 6, u->mac, 6)) return;  // (our own, looped back)
    const uint16_t type = be16(f + 12);
    if (type == 0x0806 && n >= 42) {               // ARP
        if (be16(f + 14) != 1 || be16(f + 16) != 0x0800 || f[18] != 6 || f[19] != 4) return;
        const uint32_t sip = ipAt(f + 28), tip = ipAt(f + 38);
        learn(u, sip, f + 22, now);
        if (be16(f + 20) == 1 && tip == u->ip && u->ip) { arp(u, 2, f + 22, sip); u->answered++; }
        return;
    }
    if (type != 0x0800 || n < 34) return;
    const uint8_t* ip = f + 14;
    const uint32_t ihl = (ip[0] & 15u) * 4u, total = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || 14 + total > n) return;
    if (fold(sum(0, ip, ihl)) != 0) return;        // a bad header checksum
    if ((be16(ip + 6) & 0x3fff) != 0) return;      // a fragment (none of ours need them)
    const uint32_t src = ipAt(ip + 12), dst = ipAt(ip + 16);
    if (dst != u->ip && dst != un_broadcast(u) && dst != 0xffffffffu) return;
    learn(u, src, f + 6, now);
    if (ip[9] != 17 || total < ihl + 8) return;    // UDP only
    const uint8_t* ud = ip + ihl;
    const uint32_t ulen = be16(ud + 4);
    if (ulen < 8 || ihl + ulen > total) return;
    if (be16(ud + 6)) {                            // a checksum given: check it
        uint32_t s = sum(0, ip + 12, 8);
        s += 17 + ulen;
        if (fold(sum(s, ud, ulen)) != 0) return;
    }
    if (u->udp) u->udp(u->ctx, src, be16(ud), be16(ud + 2), ud + 8, ulen - 8);
}

int un_send(un* u, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t* d, uint32_t n, uint32_t now)
{
    if (n + 28 > UN_MTU) return -2;
    const uint8_t* mac = 0;
    if (dst == un_broadcast(u) || dst == 0xffffffffu) mac = kBcast;
    else
        for (int i = 0; i < UN_ARP; i++)
            if (u->arp[i].valid && u->arp[i].ip == dst) { mac = u->arp[i].mac; u->arp[i].usedUs = now; break; }
    if (!mac) { arp(u, 1, kBcast, dst); u->asked++; u->dropped++; return -1; }
    uint8_t* f = u->out;
    copy(f, mac, 6);
    copy(f + 6, u->mac, 6);
    put16(f + 12, 0x0800);
    uint8_t* ip = f + 14;
    const uint16_t total = (uint16_t)(20 + 8 + n);
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, total); put16(ip + 4, u->ipId++); put16(ip + 6, 0x4000);   // don't fragment
    ip[8] = 64; ip[9] = 17; put16(ip + 10, 0);
    copy(ip + 12, &u->ip, 4); copy(ip + 16, &dst, 4);
    put16(ip + 10, fold(sum(0, ip, 20)));
    uint8_t* ud = ip + 20;
    put16(ud, sport); put16(ud + 2, dport); put16(ud + 4, (uint16_t)(8 + n)); put16(ud + 6, 0);
    copy(ud + 8, d, n);
    uint32_t s = sum(0, ip + 12, 8);
    s += 17u + 8u + n;
    uint16_t c = fold(sum(s, ud, 8 + n));
    put16(ud + 6, c ? c : 0xffff);                 // (0 means "none" in UDP: send its other form)
    uint32_t len = 14u + total;
    if (len < 60) { zero(f + len, 60 - len); len = 60; }
    u->tx(u->ctx, f, len);
    return 0;
}
