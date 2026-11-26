// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// prolink_seg: fw/prolink.c and fw/udpnet.c as a player on cdj-nxs2-qemu's Pro DJ Link network: QEMU's
// `-netdev socket,mcast=GROUP:PORT` segment, one raw Ethernet frame per UDP datagram, joined like the rig's
// own dhcp_server.py (no root, no tap). So the emulated Pioneer deck, running its own firmware, is the peer.
//
//   prolink_seg GROUP:PORT [seconds] [--ip 192.168.50.20] [--player 3] [--master-at S] [--play BPM] [--sync-at S]
//   prolink_seg lo ...            the loopback instead, plain UDP sockets on the three ports, us at 127.0.0.1:
//                                 beside Deckbench's simulated players (127.0.0.N; sim/prolink_bench.py)
//
// It prints every peer, the master it follows (player, tempo, beats), and our events; --master-at asks for
// tempo master S seconds in, --play sends our own status and beats as a deck playing at BPM, --sync-at S then
// syncs that deck to the master: its tempo, its beats on the master's (the simplest SYNC, for the bench to see).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "prolink.h"
#include "udpnet.h"

static int s_sock, s_lo, s_port[3];
static struct sockaddr_in s_group;
static un s_net;
static pl s_link;
static uint32_t s_now;
static uint32_t s_rx[3], s_tx[3], s_txFail;

static uint32_t nowUs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}
static void frameOut(void* c, const uint8_t* f, uint32_t n)
{
    (void)c;
    sendto(s_sock, f, n, 0, (struct sockaddr*)&s_group, sizeof s_group);
}
static void udpIn(void* c, uint32_t src, uint16_t sport, uint16_t dport, const uint8_t* d, uint32_t n)
{
    (void)c; (void)sport;
    if (dport >= PL_PORT_ANNOUNCE && dport <= PL_PORT_STATUS) { s_rx[dport - PL_PORT_ANNOUNCE]++; pl_rx(&s_link, dport, d, n, src, s_now); }
}
static void linkOut(void* c, uint32_t ip, uint16_t port, const uint8_t* d, uint32_t n)
{
    (void)c;
    if (s_lo) {
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
        if (sendto(s_port[port - PL_PORT_ANNOUNCE], d, n, 0, (struct sockaddr*)&to, sizeof to) == (ssize_t)n) s_tx[port - PL_PORT_ANNOUNCE]++;
        else s_txFail++;
        return;
    }
    if (un_send(&s_net, ip, port, port, d, n, s_now) == 0) s_tx[port - PL_PORT_ANNOUNCE]++;
    else s_txFail++;
}
static const char* ipStr(uint32_t ip) { struct in_addr a = {ip}; return inet_ntoa(a); }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: prolink_seg GROUP:PORT [seconds] [--ip A.B.C.D] [--player N] [--master-at S] [--play BPM]\n"); return 2; }
    char group[64];
    snprintf(group, sizeof group, "%s", argv[1]);
    char* colon = strrchr(group, ':');
    if (!colon && strcmp(group, "lo")) return 2;
    if (colon) *colon = 0;
    const int port = colon ? atoi(colon + 1) : 0;
    double seconds = 30, masterAt = -1, playBpm = 0, syncAt = -1;
    const char* ipArg = "192.168.50.20";
    int player = 3;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--ip") && i + 1 < argc) ipArg = argv[++i];
        else if (!strcmp(argv[i], "--player") && i + 1 < argc) player = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--master-at") && i + 1 < argc) masterAt = atof(argv[++i]);
        else if (!strcmp(argv[i], "--play") && i + 1 < argc) playBpm = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sync-at") && i + 1 < argc) syncAt = atof(argv[++i]);
        else seconds = atof(argv[i]);
    }
    s_lo = !strcmp(argv[1], "lo");
    if (s_lo) {
        ipArg = "127.0.0.1";
        for (int i = 2; i < argc - 1; i++) if (!strcmp(argv[i], "--ip")) ipArg = argv[i + 1];
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    const int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s_lo) {
        // Deckbench's way on Linux: SO_REUSEADDR only (SO_REUSEPORT would hand each datagram to one socket of the
        // port's, not to all); bound to any address, so the loopback's broadcasts reach us; its players bind their own
        close(s_sock);
        for (int i = 0; i < 3; i++) {
            s_port[i] = socket(AF_INET, SOCK_DGRAM, 0);
            setsockopt(s_port[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
            setsockopt(s_port[i], SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
            struct sockaddr_in b = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(PL_PORT_ANNOUNCE + i)), .sin_addr.s_addr = htonl(INADDR_ANY)};
            if (bind(s_port[i], (struct sockaddr*)&b, sizeof b)) { perror("bind"); return 1; }
        }
    } else if (bind(s_sock, (struct sockaddr*)&a, sizeof a)) { perror("bind"); return 1; }
    struct ip_mreq mr;
    inet_aton(group, &mr.imr_multiaddr);
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!s_lo && setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr)) { perror("membership"); return 1; }
    const unsigned char ttl = 1;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    s_group = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = mr.imr_multiaddr};

    struct in_addr ip, mask;
    inet_aton(ipArg, &ip);
    inet_aton(s_lo ? "255.0.0.0" : "255.255.255.0", &mask);
    const uint8_t mac[6] = {0x02, 0xf1, 0x00, 0x00, 0x00, (uint8_t)player};   // (locally administered, "F1")
    un_begin(&s_net, mac, ip.s_addr, mask.s_addr, frameOut, udpIn, 0);
    pl_begin(&s_link, (uint8_t)player, "FM-1", linkOut, 0);
    if (!s_lo) un_announce(&s_net);
    if (s_lo) printf("on the loopback as player %d, %s\n", player, ipArg);
    else printf("on %s:%d as player %d, %s\n", group, port, player, ipArg);
    fflush(stdout);

    const uint32_t t0 = nowUs();
    uint32_t lastPrint = t0, beatN = 0, nextBeatUs = t0 + 500000u;
    pl_master was = {0};
    int asked = 0, synced = 0;
    for (;;) {
        s_now = nowUs();
        const double t = (s_now - t0) / 1e6;
        if (t >= seconds) break;
        if (masterAt >= 0 && !asked && t >= masterAt) { asked = 1; pl_request_master(&s_link, s_now); printf("%6.2f asking for tempo master\n", t); }
        if (syncAt >= 0 && !synced && t >= syncAt) { synced = 1; printf("%6.2f SYNC on\n", t); }
        if (synced) {                              // SYNC: the master's tempo, our beats on its beats
            pl_master sm;
            if (pl_get_master(&s_link, s_now, &sm) && sm.bpm > 0 && sm.beatUs) {
                playBpm = sm.bpm;
                const uint32_t per = (uint32_t)(60e6 / sm.bpm);
                uint32_t next = sm.beatUs;
                while ((int32_t)(s_now - next) >= 0) next += per;
                if ((int32_t)(next - nextBeatUs) > (int32_t)per / 2) beatN++;   // (the grid moved on: a beat went by)
                nextBeatUs = next;
            }
        }
        if (playBpm > 0) {                         // a deck playing at playBpm: beats from 1, 4 to the bar
            const uint32_t period = (uint32_t)(60e6 / playBpm);
            while ((int32_t)(s_now - nextBeatUs) > 0) { nextBeatUs += period; beatN++; }
            pl_ours o = {.loaded = 1, .playing = 1, .sync = (uint8_t)synced, .trackBpm = (float)playBpm, .pitch = 1.0f, .beat = beatN ? beatN : 1,
                         .beatInBar = (uint8_t)(beatN ? (beatN - 1) % 4 + 1 : 1), .nextBeat = beatN + 1,
                         .nextBeatUs = nextBeatUs, .nextBeatInBar = (uint8_t)(beatN % 4 + 1)};
            pl_publish(&s_link, &o);
        }
        const uint32_t wait = pl_tick(&s_link, s_now, s_net.ip, s_net.mask, s_net.mac);
        if (s_lo) {
            struct pollfd p[3];
            for (int i = 0; i < 3; i++) p[i] = (struct pollfd){s_port[i], POLLIN, 0};
            if (poll(p, 3, (int)(wait / 1000 + 1)) > 0)
                for (int i = 0; i < 3; i++) {
                    if (!(p[i].revents & POLLIN)) continue;
                    static uint8_t d[2048];
                    struct sockaddr_in from;
                    socklen_t fl = sizeof from;
                    const ssize_t n = recvfrom(s_port[i], d, sizeof d, 0, (struct sockaddr*)&from, &fl);
                    s_now = nowUs();
                    if (n > 0) udpIn(0, from.sin_addr.s_addr, ntohs(from.sin_port), (uint16_t)(PL_PORT_ANNOUNCE + i), d, (uint32_t)n);
                }
        } else {
            struct pollfd p = {s_sock, POLLIN, 0};
            if (poll(&p, 1, (int)(wait / 1000 + 1)) > 0) {
                static uint8_t f[2048];
                const ssize_t n = recv(s_sock, f, sizeof f, 0);
                s_now = nowUs();
                if (n > 0) un_rx(&s_net, f, (uint32_t)n, s_now);
            }
        }
        uint8_t arg;
        int e;
        while ((e = pl_event(&s_link, &arg)) != PL_EV_NONE) {
            static const char* names[] = {"", "new player", "we are tempo master (1 nobody had it, 2 handed over):", "handed master to player",
                                          "yielding master to player", "player agreed to yield:", "asking the master, player", "handoff timed out, master",
                                          "sync control from player", "on air:"};
            printf("%6.2f %s %u\n", t, names[e], arg);
            if (e == PL_EV_PEER)
                for (int i = 0; i < PL_MAX_PEERS; i++)
                    if (s_link.peers[i].seen && s_link.peers[i].player == arg) printf("       player %u at %s\n", arg, ipStr(s_link.peers[i].ip));
        }
        pl_master m;
        pl_get_master(&s_link, s_now, &m);
        if (m.present != was.present || m.player != was.player || m.beating != was.beating || m.playing != was.playing ||
            (m.bpm - was.bpm > 0.005f || was.bpm - m.bpm > 0.005f)) {
            if (m.present) printf("%6.2f master: player %u @ %.2f BPM, %s, %s\n", t, m.player, m.bpm, m.playing ? "playing" : "stopped",
                                  m.beating ? "beating" : "no beats");
            else printf("%6.2f master: none%s\n", t, pl_is_master(&s_link) ? " (we are it)" : "");
            was = m;
        }
        if (s_now - lastPrint >= 5000000u) {
            lastPrint = s_now;
            printf("%6.2f rx announce %u beat %u status %u; tx %u %u %u (%u without ARP); peers %d%s\n", t, s_rx[0], s_rx[1], s_rx[2],
                   s_tx[0], s_tx[1], s_tx[2], s_txFail, pl_peers(&s_link, s_now), pl_is_master(&s_link) ? "; WE ARE MASTER" : "");
        }
        fflush(stdout);
    }
    printf("end: rx announce %u beat %u status %u; tx announce %u beat %u status %u; ARP asked %u answered %u\n", s_rx[0], s_rx[1],
           s_rx[2], s_tx[0], s_tx[1], s_tx[2], s_net.asked, s_net.answered);
    return 0;
}
