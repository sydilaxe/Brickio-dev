/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mclockio: the bytes in (song position, the link position, split however the reads come), the app's lock
 * held around every change, the relay (a following app passing the clock on, never back to its source),
 * and the threads for real through a FIFO standing in for a MIDI port. */
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/linkpos.h"
#include "../src/mclockio.h"
#include "../src/midi.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the app: its changes counted, and whether each came with the lock held */
static volatile float bpm = 100.f;
static volatile double pos;
static atomic_int playing, held, changes, unheld;
static void lock(void) { if (atomic_exchange(&held, 1)) atomic_fetch_add(&unheld, 1000); }
static void unlock(void) { atomic_store(&held, 0); }
static void changed(void) { atomic_fetch_add(&changes, 1); if (!atomic_load(&held)) atomic_fetch_add(&unheld, 1); }
static float h_bpm(void) { return bpm; }
static void h_set_bpm(float b) { changed(); bpm = b; }
static double h_pos(void) { return pos; }
static void h_set_pos(double p) { changed(); pos = p; }
static void h_rate(double r) { (void)r; changed(); }
static int h_playing(void) { return playing; }
static void h_set_playing(int on) { changed(); if (on && !playing) pos = 0.0; playing = on; }
static const ClockHost HOST = {h_bpm, h_set_bpm, h_pos, h_set_pos, h_rate, h_playing, h_set_playing};

static void nap(int ms) { struct timespec ts = {0, ms * 1000000L}; nanosleep(&ts, NULL); }
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000; }

int main(void)
{
    McioConfig c = {.host = &HOST, .lock = lock, .unlock = unlock};
    mcio_init(&c);
    mclock_set_latency(0.f);

    /* song position, its bytes in three reads, a note's in the middle of nothing (passed by), then CONTINUE
     * and ticks: the app plays from 16th 32 */
    uint64_t t = 1000000;
    for (int i = 0; i < 48; i++, t += 20833) { const uint8_t tick = 0xf8; mcio_feed(&tick, 1, t); } /* a running clock: the tempo */
    const uint8_t a[] = {0x90, 60, 100, 0xf2}, b1[] = {32}, b2[] = {0}, cont[] = {0xfb};
    { const uint8_t tk = 0xf8; mcio_feed(&tk, 1, t); } /* the clock runs on: the song position comes between ticks */
    mcio_feed(a, sizeof a, t), mcio_feed(b1, 1, t), mcio_feed(b2, 1, t);
    mcio_feed(cont, 1, t + 5000);
    for (int i = 0; i < 6; i++) { t += 20833; const uint8_t tick = 0xf8; mcio_feed(&tick, 1, t); }
    /* (this app doesn't move between ticks: the first tick after CONTINUE places it at 32 exactly) */
    printf("song position in pieces: plays %d, at %.3f 16ths (want 32), the tempo %.2f\n", (int)playing,
           (double)pos, (double)bpm);
    CHECK(playing && fabs(pos - 32.0) < 0.01, "song position then CONTINUE: from 16th 32 (%.3f)", (double)pos);
    CHECK(fabs(bpm - 120.f) < 0.2f, "the tempo from the running clock (%.2f)", (double)bpm);

    /* F1 and F3 take a byte each: it must not be read as anything */
    const uint8_t common[] = {0xf3, 5, 0xf1, 0x22};
    mcio_feed(common, sizeof common, t);
    CHECK(playing && fabs(pos - 32.0) < 0.01, "F1 / F3 and their bytes passed by");
    const uint8_t stop = 0xfc;
    mcio_feed(&stop, 1, t + 1000);
    CHECK(!playing, "STOP");

    /* the link position, split over two reads, a tick in the middle (real time goes anywhere) */
    mclock_reset();
    uint8_t lp[LINKPOS_LEN];
    int n = linkpos_encode(lp, sizeof lp, 64.0, 137.5, 1);
    mcio_feed(lp, 6, 9000000);
    const uint8_t tick = 0xf8;
    mcio_feed(&tick, 1, 9000000);
    mcio_feed(lp + 6, n - 6, 9000100);
    CHECK(playing && fabs(bpm - 137.5f) < 0.01f && pos > 63.9, "the link position in two pieces: joined at 16th %.2f, %.2f BPM", (double)pos,
          (double)bpm);
    CHECK(atomic_load(&unheld) == 0 && atomic_load(&changes) > 0, "every change made with the lock held (%d changes, %d without)",
          (int)changes, (int)unheld);

    /* the relay: following, the clock goes on only with relay on */
    mclock_reset(), playing = 0;
    uint8_t out[32];
    const uint8_t start = 0xfa;
    t = 20000000;
    mcio_feed(&start, 1, t);
    int sent_off = 0, sent_on = 0;
    for (int i = 0; i < 200; i++) {
        t += 20833;
        mcio_feed(&tick, 1, t);
        pos += 1.0 / 6.0; /* the app moving with the clock */
        if (i == 100) mclock_set_relay(1);
        int k = mclock_step(t + 100, out, sizeof out);
        for (int j = 0; j < k; j++) (i < 100 ? &sent_off : &sent_on)[0] += out[j] == 0xf8 || out[j] == 0xfa;
    }
    printf("relay: following, %d bytes out without it, %d with it (want 0 and about 100)\n", sent_off, sent_on);
    CHECK(mclock_following() && sent_off == 0 && sent_on > 90, "relay: the clock passed on only when asked (%d, %d)", sent_off, sent_on);
    playing = 0;
    n = mclock_step(t + 2000, out, sizeof out);
    CHECK(n == 1 && out[0] == 0xfc, "relay: STOP passed on");
    mclock_set_relay(0);

    /* the threads, for real: a FIFO as a MIDI port, clock written into it; the reader stamps it, mclock
     * follows, and the relayed clock never goes back into the port it came from */
    char dir[] = "/tmp/brickio-mcioXXXXXX", path[64];
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof path, "%s/midiC9D0", dir);
    if (mkfifo(path, 0600)) { printf("no FIFO: the threads skipped\n"); }
    else {
        mclock_reset(), playing = 0;
        bpm = 100.f;
        McioConfig cm = {.host = &HOST, .lock = lock, .unlock = unlock, .midi = 1};
        mcio_init(&cm);
        mclock_set_relay(1);
        midi_set_priority(40);
        midi_start(path, NULL);
        for (int i = 0; i < 50 && !midi_count(); i++) nap(20);
        mcio_start();
        int w = open(path, O_WRONLY | O_NONBLOCK);
        const uint8_t st = 0xfa;
        if (w < 0 || write(w, &st, 1) != 1) { printf("write failed\n"); return 1; }
        uint64_t t0 = now_us();
        int wrote = 0;
        for (int i = 0; i < 96; i++) { /* 2 s at 120 BPM */
            uint64_t due = t0 + (uint64_t)((i + 1) * 20833);
            while (now_us() < due) nap(1);
            if (write(w, &tick, 1) == 1) wrote++;
        }
        nap(100);
        MidiEv ev[256];
        int got = 0, k;
        while ((k = midi_poll(ev, 256)) > 0)
            for (int j = 0; j < k; j++) got += ev[j].type == MIDI_CLOCK;
        printf("threads: %d ticks written, %d read (a relayed clock fed back would add more), following %d, playing %d, the tempo %.2f\n",
               wrote, got, mclock_following(), (int)playing, (double)bpm);
        CHECK(got == wrote, "the relay leaves out the port the clock comes from (%d read, %d written)", got, wrote);
        CHECK(mclock_following() && playing && fabs(bpm - 120.f) < 1.f, "following through the reader thread (%.2f BPM)", (double)bpm);
        mcio_stop();
        midi_stop();
        close(w);
        unlink(path);
    }
    rmdir(dir);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
