// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// usbmsc: a USB stick on the FM-1's own port (AC791N USB0 as a host), read as a drive: the controller set
// up as JieLi's AC79 SDK driver does, plus what the FM-1 showed it needs (CON0 bit 3 for the bus to start,
// the session restarted with the stick there, 50 us for the DMA either side of a packet, the RX FIFO flushed
// after each); the stick enumerated; Bulk-Only Transport (READ(10)). Polled, no interrupts; the caller
// keeps everything else off the controller (fm1-dock's own USB: see hal_fm1.c's FM1_STICK, and the probes,
// which turn the interrupts off). Found with probes usbhost.c / usbstick.c on a real FM-1, 2026-11-25.
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t (*ticks)(void);                     // a 24 MHz counter (TIMER4: hb_host_t.ticks, probe.h p_ticks)
    void (*idle)(void);                          // called while waiting (a probe feeds the watchdog), or 0
} umsc_env;

enum { UMSC_OFF, UMSC_WAIT, UMSC_READY, UMSC_FAIL };

void     umsc_host(const umsc_env* e);            // the controller as a host, the session asked for: UMSC_WAIT
int      umsc_attached(void);                     // a device on the port (D+ / D- up, the core's FSDev / LSDev)
int      umsc_open(void);                         // reset, enumerate, find bulk-only mass storage (on the port,
                                                  //  or on a full-speed hub's first port that has one), INQUIRY,
                                                  //  READ CAPACITY: 0 (UMSC_READY), < 0 the step that failed, 1 no
                                                  //  drive but MIDI or audio on the port itself (umsc_audio ...)
int      umsc_read(uint32_t lba, uint32_t n, uint8_t* dst);   // n blocks of 512 (n <= 64): 0, < 0 an error
int      umsc_present(void);                      // the device still there (not SE0 for 100 ms): 0 once it's pulled out
void     umsc_close(void);
int      umsc_midi(uint8_t* dst);                 // a USB-MIDI device found on a hub too (a DJ2Go): one poll, its
                                                  //  bytes (4-byte USB-MIDI packets, <= 64) into dst; 0 none / no device                        // the controller off (CON0 / IO as at reset)

extern uint32_t umsc_state, umsc_blocks, umsc_vid, umsc_pid, umsc_err, umsc_recoveries;
typedef struct {                                  // a USB audio input found (ep 0: none): its address, EP0 size,
    uint32_t addr, maxp0, ifc, alt, ep, maxp;     //  AudioStreaming interface + alternate setting, iso IN endpoint,
    uint32_t ch, bytes, bits, rate, id;           //  format (channels, bytes a sample, bits, the top rate <= 48k), VID << 16 | PID
} umsc_audio_t;
extern umsc_audio_t umsc_audio;
// streaming it, on interrupts (vectors 8 and 9): SET_INTERFACE, the iso IN endpoint asked every frame into a ring
// (85 ms). umsc_read holds it around each read (the controller has one device address). 0, < 0 failed
int      umsc_audio_start(void);
void     umsc_audio_stop(void);
uint32_t umsc_audio_read(int16_t* lr, uint32_t frames);   // up to frames of L, R (16 bit) out of the ring: the count
uint32_t umsc_audio_avail(void);
int      umsc_audio_present(void);                 // still plugged in, without using the bus (0: no packets for 200 ms)
extern uint32_t umsc_audio_con0;
extern volatile uint32_t umsc_apk, umsc_amiss, umsc_aerr, umsc_aover;   // packets, re-asks, errors, ring overflows
extern volatile uint32_t umsc_aheld, umsc_aheldMax;    // the stream held for another device's transfer (umsc_read on
                                                  //  a hub with it: the stick), the longest hold (24 MHz ticks)
extern uint32_t umsc_midiid;                     // the MIDI device's VID << 16 | PID (0: none)
extern uint32_t umsc_hub, umsc_hubid, umsc_trace[32], umsc_ntrace;          // the hub port the drive is on (0: no hub), the hub's VID << 16 | PID
extern char     umsc_name[25];                    // INQUIRY's vendor + product
