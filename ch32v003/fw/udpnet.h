// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// udpnet: just enough IPv4 for Pro DJ Link over raw Ethernet frames: ARP (answers, asks, learns from every IPv4
// frame), IPv4 without fragments, UDP with checksums. No OS, no libc, no TCP (Pro DJ Link's beat, status and
// announce are all UDP). Frames in through un_rx, out through a callback: a USB Ethernet adapter's bulk
// endpoints on the FM-1 (CDC-ECM), or QEMU's multicast segment on the host (sim/prolink_seg.c, beside the
// emulated Pioneer decks of cdj-nxs2-qemu). Addresses are uint32s holding the four bytes in wire order.
#pragma once
#include <stdint.h>

// UN_MTU: the biggest datagram + 28 we send. Pro DJ Link's biggest is a status (0x124 bytes), so a tiny device
// can build with -DUN_MTU=576 (the frame buffer is most of un's RAM: ~1.7 KB at 1500, ~0.8 KB at 576).
#ifndef UN_MTU
#define UN_MTU 1500
#endif
enum { UN_ARP = 8, UN_FRAME = 14 + UN_MTU };

typedef void (*un_frame_fn)(void* ctx, const uint8_t* frame, uint32_t n);
typedef void (*un_udp_fn)(void* ctx, uint32_t srcIp, uint16_t srcPort, uint16_t dstPort, const uint8_t* d, uint32_t n);

typedef struct {
    uint8_t  mac[6];
    uint32_t ip, mask;
    un_frame_fn tx;
    un_udp_fn   udp;
    void*    ctx;
    struct { uint32_t ip, usedUs; uint8_t mac[6], valid; } arp[UN_ARP];
    uint16_t ipId;
    uint32_t asked, answered, dropped;            // ARP requests we sent / answered; datagrams without a MAC yet
    uint8_t  out[UN_FRAME];
} un;

void un_begin(un* u, const uint8_t mac[6], uint32_t ip, uint32_t mask, un_frame_fn tx, un_udp_fn udp, void* ctx);
void un_rx(un* u, const uint8_t* f, uint32_t n, uint32_t nowUs);
// a datagram from srcPort to ip:dstPort: 0 sent; -1 the MAC isn't known yet (an ARP request went out instead,
// and the datagram is dropped: Pro DJ Link resends everything soon); -2 too big
int  un_send(un* u, uint32_t ip, uint16_t srcPort, uint16_t dstPort, const uint8_t* d, uint32_t n, uint32_t nowUs);
// gratuitous ARP: tell the segment our address (at start)
void un_announce(un* u);
uint32_t un_broadcast(const un* u);
