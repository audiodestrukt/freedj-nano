// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sd_emu.h"

enum { M_CMD, M_WAIT, M_DATA, M_CRC };

void sd_emu_reset(sd_emu* c)
{
    c->sel = 0; c->idle = 1; c->app = 0; c->nin = 0; c->nresp = c->iresp = 0;
    c->mode = M_CMD; c->multi = 0; c->startRead = 0; c->nreads = 0;
    if (!c->initPolls) c->initPolls = 3;
}

void sd_emu_select(sd_emu* c, int on)
{
    c->sel = (uint8_t)on;
    if (!on && !c->multi && c->mode != M_CMD) c->mode = M_CMD;   // a single read abandoned
}

static void respond(sd_emu* c, const uint8_t* r, uint8_t n)
{
    c->resp[0] = 0xFF;                                    // Ncr: one byte before the response
    for (uint8_t i = 0; i < n; i++) c->resp[1 + i] = r[i];
    c->nresp = n + 1; c->iresp = 0;
}

static void load(sd_emu* c)
{
    if (c->read(c->ctx, c->lba, c->blk)) c->errors++;
    c->blocksOut++;
}

static void exec(sd_emu* c)
{
    const uint8_t cmd = c->in[0] & 0x3F;
    const uint32_t arg = (uint32_t)c->in[1] << 24 | (uint32_t)c->in[2] << 16 | (uint32_t)c->in[3] << 8 | c->in[4];
    const uint8_t app = c->app;
    c->app = 0;
    c->cmds++;
    uint8_t r[6];
    r[0] = c->idle;
    switch (cmd) {
    case 0: c->idle = 1; c->multi = 0; c->mode = M_CMD; r[0] = 1; respond(c, r, 1); return;
    case 8: r[1] = 0; r[2] = 0; r[3] = (arg >> 8) & 15; r[4] = arg & 0xFF; respond(c, r, 5); return;
    case 55: c->app = 1; respond(c, r, 1); return;
    case 41:
        if (!app) break;
        if (c->initPolls) c->initPolls--; else c->idle = 0;
        r[0] = c->idle; respond(c, r, 1); return;
    case 58:
        r[1] = c->idle ? 0x00 : 0xC0; r[2] = 0xFF; r[3] = 0x80; r[4] = 0; respond(c, r, 5); return;
    case 16: respond(c, r, 1); return;
    case 12: {                                            // stop: stuff byte, R1, busy
        c->multi = 0; c->mode = M_CMD;
        const uint8_t s[] = { 0xFF, 0x00, 0x00, 0x00 };
        respond(c, s, 4); return;
    }
    case 17: case 18:
        if (c->idle) break;
        if (arg >= c->blocks) { r[0] = 0x40; respond(c, r, 1); return; }   // address error
        r[0] = 0;
        respond(c, r, 1);
        c->lba = arg; c->multi = cmd == 18; c->startRead = 1; c->reads++;
        c->wait = c->access;
        if (c->stallEvery && ++c->nreads % c->stallEvery == 0) { c->wait += c->stall; c->stalls++; }
        load(c);
        return;
    }
    r[0] = c->idle | 0x04;                                // illegal command
    respond(c, r, 1);
}

uint8_t sd_emu_xfer(sd_emu* c, uint8_t in)
{
    c->bytes++;
    if (!c->sel) return 0xFF;
    uint8_t out = 0xFF;
    if (c->nresp) {
        out = c->resp[c->iresp++];
        if (c->iresp == c->nresp) {
            c->nresp = 0;
            if (c->startRead) { c->startRead = 0; c->mode = M_WAIT; }
        }
    } else if (c->mode == M_WAIT) {
        if (c->wait) c->wait--;
        else { out = 0xFE; c->mode = M_DATA; c->di = 0; }
    } else if (c->mode == M_DATA) {
        out = c->blk[c->di++];
        if (c->di == 512) { c->mode = M_CRC; c->di = 0; }
    } else if (c->mode == M_CRC) {
        out = 0x5A;
        if (++c->di == 2) {
            if (c->multi && c->lba + 1 < c->blocks) { c->lba++; load(c); c->wait = c->gap; c->mode = M_WAIT; }
            else c->mode = M_CMD;
        }
    }
    // commands come in on MOSI whatever is going out (CMD12 arrives during a stream)
    if (c->nin) {
        c->in[c->nin++] = in;
        if (c->nin == 6) { c->nin = 0; exec(c); }
    } else if ((in & 0xC0) == 0x40) {
        c->in[0] = in; c->nin = 1;
    }
    return out;
}
