/* SPDX-License-Identifier: GPL-3.0-or-later */
/* midi.c, fed through a FIFO standing in for a raw MIDI port: messages come out of midi_poll (running
 * status, real-time bytes in the middle of a message), the raw tap sees every byte as read, and the
 * SysEx hook gets the link's position message whole. */
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/linkpos.h"
#include "../src/midi.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static atomic_int raw_bytes, sysex_ok;
static void raw(int dev, const uint8_t *b, int n, uint64_t t, void *ud) { (void)dev, (void)b, (void)t, (void)ud; atomic_fetch_add(&raw_bytes, n); }
static void sysex(int dev, const uint8_t *m, int n, uint64_t t, void *ud)
{
    (void)dev, (void)t, (void)ud;
    double h, bpm;
    int pl;
    if (linkpos_decode(m, n, &h, &bpm, &pl) && h == 12.5 && bpm > 123.45 && bpm < 123.46 && pl == 1) atomic_store(&sysex_ok, 1);
}
static void nap(int ms) { struct timespec ts = {0, ms * 1000000L}; nanosleep(&ts, NULL); }

int main(void)
{
    char dir[] = "/tmp/brickio-midiXXXXXX", path[64];
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof path, "%s/midiC9D0", dir);
    if (mkfifo(path, 0600)) { printf("no FIFO: skipped\n"); return 0; }
    midi_set_raw(raw, NULL);
    midi_set_sysex(sysex, NULL);
    midi_start(path, NULL);
    for (int i = 0; i < 50 && !midi_count(); i++) nap(20);
    CHECK(midi_count() == 1, "the port opened (%d)", midi_count());
    int w = open(path, O_WRONLY | O_NONBLOCK);
    uint8_t pos[LINKPOS_LEN];
    linkpos_encode(pos, sizeof pos, 12.5, 123.456, 1);
    /* note on, a clock tick in the middle of a CC, the CC's second byte, running status, then the position */
    const uint8_t a[] = {0x90, 60, 100, 0xb1, 7, 0xf8, 64, 8, 32};
    if (w < 0 || write(w, a, sizeof a) != (ssize_t)sizeof a || write(w, pos, sizeof pos) != (ssize_t)sizeof pos) { printf("write failed\n"); return 1; }
    MidiEv ev[16];
    int n = 0;
    for (int i = 0; i < 50 && n < 4; i++) { nap(20); n += midi_poll(ev + n, 16 - n); }
    CHECK(n == 4, "four messages (%d)", n);
    if (n == 4) {
        CHECK(ev[0].type == MIDI_NOTE && ev[0].num == 60 && ev[0].val == 100, "note on 60");
        CHECK(ev[1].type == MIDI_CLOCK, "the clock tick, as it came, in the middle of the CC");
        CHECK(ev[2].type == MIDI_CC && ev[2].ch == 1 && ev[2].num == 7 && ev[2].val == 64, "CC 7 = 64 on channel 2");
        CHECK(ev[3].type == MIDI_CC && ev[3].num == 8 && ev[3].val == 32, "running status: CC 8 = 32");
        CHECK(ev[0].t_ns > 0, "stamped with the time");
    }
    for (int i = 0; i < 50 && !atomic_load(&sysex_ok); i++) nap(20);
    CHECK(atomic_load(&sysex_ok), "the SysEx hook got the link position whole");
    CHECK(atomic_load(&raw_bytes) == (int)(sizeof a + sizeof pos), "the raw tap saw every byte (%d)", atomic_load(&raw_bytes));
    close(w);
    midi_stop();
    unlink(path), rmdir(dir);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
