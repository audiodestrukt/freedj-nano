// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// prolink_replay: a freedj-3000 capture log (its debug lines "ProDJ rx :PORT N bytes from A.B.C.D:P — [51, 73,
// ...]", reference/link-captures/) through fw/prolink.c at the log's own times. After each packet, one line from
// what pl understood (the peer's table entry), in the format of the f3kdump tool built on freedj-3000's own
// parser, so the two diff packet by packet. Then a summary: the master pl follows, as the log ends.
//   prolink_replay capture.log [--summary]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prolink.h"

static void nosend(void* c, uint32_t ip, uint16_t port, const uint8_t* d, uint32_t n) { (void)c; (void)ip; (void)port; (void)d; (void)n; }

static const pl_peer* peer(const pl* l, uint8_t player)
{
    for (int i = 0; i < PL_MAX_PEERS; i++) if (l->peers[i].seen && l->peers[i].player == player) return &l->peers[i];
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const int summary = argc > 2 && !strcmp(argv[2], "--summary");
    FILE* f = fopen(argv[1], "r");
    if (!f) return 1;
    static pl l;
    pl_begin(&l, 7, "REPLAY", nosend, 0);         // (player 7: not one in the captures)
    static char line[1 << 16];
    uint32_t t0 = 0, now = 0, packets = 0, beats = 0, statuses = 0, twins = 0;
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        const char* rx = strstr(line, "ProDJ rx :");
        const char* ts = strchr(line, 'T');
        if (!rx || !ts) continue;
        int hh, mm; double ss;
        if (sscanf(ts + 1, "%d:%d:%lf", &hh, &mm, &ss) != 3) continue;
        const uint32_t us = (uint32_t)((hh * 3600 + mm * 60) * 1000000.0 + ss * 1e6);
        if (first) { t0 = us; first = 0; }
        now = us - t0 + 1000000u;
        unsigned port, a, b, c, d;
        const char* from = strstr(rx, "from ");
        if (sscanf(rx + 10, "%u", &port) != 1 || !from || sscanf(from + 5, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
        const uint8_t q[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
        uint32_t ip; memcpy(&ip, q, 4);
        const char* open = strchr(rx, '[');
        static uint8_t pkt[2048];
        uint32_t n = 0;
        for (const char* p = open ? open + 1 : 0; p && *p && *p != ']' && n < sizeof pkt;) {
            char* e;
            const unsigned long v = strtoul(p, &e, 16);
            if (e == p) break;
            pkt[n++] = (uint8_t)v;
            p = e;
            while (*p == ',' || *p == ' ') p++;
        }
        const uint32_t beatUsWas = n > 0x21 && peer(&l, pkt[0x21]) ? peer(&l, pkt[0x21])->beatUs : 0;
        pl_rx(&l, (uint16_t)port, pkt, n, ip, now);
        pl_tick(&l, now, 0, 0, 0);
        packets++;
        if (summary) continue;
        const int type = n > 0x0a ? pkt[0x0a] : -1;
        if (port == PL_PORT_BEAT && type == 0x28) {
            const pl_peer* p = peer(&l, pkt[0x21]);
            if (!p || !p->haveBeat) { printf("beat rejected %u\n", n); continue; }
            beats++;
            if (p->beatUs == beatUsWas && p->beatUs != now) twins++;   // (dropped as a twin: its state is the last's)
            printf("beat player %u bpm %.2f bib %u\n", p->player, p->beatBpm, p->beatInBar);
        } else if (port == PL_PORT_STATUS && type == 0x0a) {
            const pl_peer* p = peer(&l, pkt[0x21]);
            if (!p || !p->haveStatus || p->statusUs != now) { printf("status rejected %u\n", n); continue; }
            statuses++;
            printf("status player %u playing %u master %u bpm %.2f pitch %.4f handoff %u syncN %u\n", p->player, p->playing,
                   p->master, p->trackBpm, p->pitch, p->handoffTo == 0 ? 0xff : p->handoffTo, p->syncN);
        } else if (port == PL_PORT_ANNOUNCE && type == 0x06) {
            const pl_peer* p = n > 0x24 ? peer(&l, pkt[0x24]) : 0;
            if (!p) { printf("announce rejected %u\n", n); continue; }
            const uint8_t* o = (const uint8_t*)&p->ip;
            printf("announce player %u ip %u.%u.%u.%u\n", p->player, o[0], o[1], o[2], o[3]);
        } else {
            printf("other port %u type %d len %u\n", port, type, n);
        }
    }
    pl_master m;
    const int has = pl_get_master(&l, now, &m);
    fprintf(summary ? stdout : stderr, "%u packets (%u beats, %u statuses, %u twins dropped); %u s; master: %s", packets, beats,
            statuses, twins, packets ? (now - 1000000u) / 1000000u : 0, has ? "" : "none\n");
    if (has) fprintf(summary ? stdout : stderr, "player %u @ %.2f BPM, %s, %s\n", m.player, m.bpm, m.playing ? "playing" : "not playing",
                     m.beating ? "beating" : "no beats");
    return 0;
}
