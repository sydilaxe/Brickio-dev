/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mclock against a simulated app: an engine whose position runs at its tempo and speed trim, heard a
 * latency later. Clock in with USB-like jitter (tempo found, the heard sound locked to the ticks),
 * a tempo change, a leading Brick's link positions, the clock out (ticks fall with the heard sound),
 * START / STOP, the clock only, and an outside clock that stops. Errors are in milliseconds of sound. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/linkpos.h"
#include "../src/mclock.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- the simulated app */
static float bpm = 100.f;
static double pos, rate = 1.0;
static int playing;
static const double LAT = 0.020; /* the output's latency, s */
static float h_bpm(void) { return bpm; }
static void h_set_bpm(float b) { bpm = b; }
static double h_pos(void) { return pos; }
static void h_set_pos(double p) { pos = p; }
static void h_rate(double r) { rate = r; }
static int h_playing(void) { return playing; }
static void h_set_playing(int on) { if (on && !playing) pos = 0.0; playing = on; }
static const ClockHost HOST = {h_bpm, h_set_bpm, h_pos, h_set_pos, h_rate, h_playing, h_set_playing};

static double heard(void) { return pos - LAT * bpm / 15.0; }
static double steps_ms(double steps, double b) { return steps * 15000.0 / b; }

/* the engine, dt seconds on */
static void run(double dt)
{
    if (playing) pos += dt * bpm / 15.0 * rate;
}

static unsigned rs = 12345;
static double jitter(double ms) { rs = rs * 1103515245u + 12345u; return ((rs >> 8) / 16777216.0 - 0.5) * 2.0 * ms * 1000.0; } /* us */

/* a leader sending clock at lb BPM from t0 for secs; returns the worst phase error (ms) over the
 * last `measure` seconds, and the mean */
/* a leader at lb BPM, as MIDI 1.0 has it: START, then the first tick on the downbeat and one every
 * 16th / 6 (each arriving 0-jit ms late). pre: seconds of clock before START (a drum machine's
 * clock runs all the time; a sequencer's starts with START: 0). Returns the worst phase error (ms)
 * over the last `measure` seconds: the follower's heard sound against the leader's, at each tick */
static int drop_every; /* 0: every tick arrives; n: every nth is lost on the way (a busy hub) */
static double follow_clock2(double lb, double lb2, double pre, double secs, double measure, double jit_ms, double *mean)
{
    static uint64_t t0 = 10000000;
    double tick_us = 60e6 / (lb * 24.0), worst = 0.0, sum = 0.0;
    int n = 0;
    uint64_t start = t0 + (uint64_t)(pre * 1e6), t = t0;
    long k = -(long)floor(pre * 1e6 / tick_us); /* tick k marks the leader's 16th k / 6 (0: the downbeat) */
    double next = (double)start + k * tick_us; /* when tick k goes out */
    uint64_t sent_at = (uint64_t)next, arrives = sent_at;
    int started = 0;
    for (; t < start + (uint64_t)(secs * 1e6); t += 50) { /* 50 us steps */
        run(5e-5);
        if (!started && t >= start) mclock_realtime(0xfa, t), started = 1;
        if (k > 0 && t >= sent_at && t - sent_at < 50 && t > start + (uint64_t)((secs - measure) * 1e6)) {
            double e = fabs(steps_ms(heard() - k / 6.0, 60e6 / (tick_us * 24.0)));
            worst = e > worst ? e : worst, sum += e, n++;
        }
        if (t >= arrives) {
            if (!drop_every || k % drop_every != drop_every - 1) mclock_realtime(0xf8, t); /* stamped as it arrives, as the MIDI thread does */
            k++;
            if (t > start + (uint64_t)(secs * 1e6 / 3)) tick_us = 60e6 / (lb2 * 24.0); /* the leader's tempo changes, playing on */
            next += tick_us;
            sent_at = (uint64_t)next;
            arrives = sent_at + (uint64_t)(jit_ms > 0 ? fabs(jitter(jit_ms)) : 0);
        }
        mclock_watch(t);
    }
    t0 = t + 100000;
    if (mean) *mean = n ? sum / n : 0.0;
    return worst;
}

static double follow_clock(double lb, double pre, double secs, double measure, double jit_ms, double *mean)
{
    return follow_clock2(lb, lb, pre, secs, measure, jit_ms, mean);
}

int main(void)
{
    mclock_init(&HOST);
    mclock_set_latency((float)LAT);

    /* clock in, 120 BPM, USB-like jitter (0-1 ms late) */
    double mean, w = follow_clock(120.0, 0.0, 30.0, 10.0, 1.0, &mean);
    printf("clock in at 120 BPM (starts with START), 1 ms jitter: the tempo %.2f, within %.3f ms of the leader (mean %.3f)\n", bpm, w, mean);
    CHECK(fabs(bpm - 120.f) < 0.1f, "finds 120 BPM (%.2f)", bpm);
    CHECK(mclock_following() && playing, "following, playing");
    CHECK(mean < 0.75 && w < 1.5, "locked: mean %.3f ms, worst %.3f ms (want under 0.75 and 1.5 with up to 1 ms of jitter)", mean, w);

    /* a faster tempo, 250 BPM */
    mclock_reset(), playing = 0, bpm = 100.f;
    w = follow_clock(250.0, 0.0, 30.0, 10.0, 1.0, &mean);
    printf("clock in at 250 BPM: the tempo %.2f, within %.3f ms (mean %.3f)\n", bpm, w, mean);
    CHECK(fabs(bpm - 250.f) < 0.2f && mean < 0.75 && w < 1.5, "250 BPM locked (%.2f, mean %.3f)", bpm, mean);

    /* the leader changes tempo while playing: 120 -> 132 at 10 s; measured over the last 10 of 30 s */
    mclock_reset(), playing = 0, bpm = 100.f;
    w = follow_clock2(120.0, 132.0, 0.0, 30.0, 10.0, 1.0, &mean);
    printf("tempo change 120 -> 132 while playing: the tempo %.2f, within %.3f ms (mean %.3f)\n", bpm, w, mean);
    CHECK(fabs(bpm - 132.f) < 0.15f && mean < 0.75 && w < 1.5, "132 BPM locked after the change (%.2f, mean %.3f)", bpm, mean);

    /* a clock that runs all the time (a drum machine), 3 s before START: the first tick after START is
     * still the downbeat, not the one after it */
    mclock_reset(), playing = 0, bpm = 100.f;
    w = follow_clock(120.0, 3.0, 20.0, 8.0, 1.0, &mean);
    printf("clock in at 120 BPM, running before START: the tempo %.2f, within %.3f ms (mean %.3f)\n", bpm, w, mean);
    CHECK(fabs(bpm - 120.f) < 0.15f && mean < 0.75 && w < 1.5, "a clock running before START: locked, not a tick off (mean %.3f ms)", mean);

    /* ticks lost on the way: one in 40 (a busy hub), 1 ms of jitter. Counted all the same: the heard sound
     * stays on the right tick, and the tempo isn't pulled down */
    mclock_reset(), playing = 0, bpm = 100.f;
    drop_every = 40;
    w = follow_clock(140.0, 1.0, 20.0, 8.0, 1.0, &mean);
    drop_every = 0;
    printf("clock in at 140 BPM, one tick in 40 lost: the tempo %.2f, within %.3f ms (mean %.3f)\n", bpm, w, mean);
    CHECK(fabs(bpm - 140.f) < 0.15f && mean < 0.75 && w < 1.5, "lost ticks: still locked to the right tick (%.2f BPM, mean %.3f ms)", bpm, mean);

    /* STOP stops; the outside clock gone quiet is let go */
    mclock_realtime(0xfc, 50000000);
    CHECK(!playing, "STOP stops the app");
    mclock_watch(52000000);
    CHECK(!mclock_following() && rate == 1.0, "a clock quiet for a second is let go, the speed back to 1");

    /* the clock only: START ignored */
    mclock_reset(), playing = 0;
    mclock_set_transport(0);
    mclock_realtime(0xfa, 60000000);
    CHECK(!playing, "the clock only: START doesn't start the app");
    mclock_set_transport(1);

    /* a leading Brick's link positions, every 20 ms, 0.3 ms in transit */
    mclock_reset(), playing = 0, bpm = 100.f;
    {
        double lb = 137.37, lead = 37.0, worst = 0.0;
        uint64_t t = 70000000;
        for (int i = 0; i < 1500; i++) { /* 30 s */
            uint8_t msg[LINKPOS_LEN];
            double hh, bb;
            int pl;
            linkpos_encode(msg, sizeof msg, lead, lb, 1);
            /* 0.3 ms in transit: it arrives (and is stamped) after that much sound */
            for (int s = 0; s < 3; s++) run(1e-4), t += 100, lead += 1e-4 * lb / 15.0;
            if (linkpos_decode(msg, sizeof msg, &hh, &bb, &pl)) mclock_link_position(t, hh, bb, pl);
            for (int s = 3; s < 200; s++) run(1e-4), t += 100, lead += 1e-4 * lb / 15.0;
            if (i > 1000) { /* the last 10 s: the leader's heard sound against ours */
                double e = fabs(steps_ms(heard() - lead, lb));
                worst = e > worst ? e : worst;
            }
        }
        printf("link: joins mid-song %d, the tempo %.2f, within %.3f ms of the leader\n", playing, bpm, worst);
        CHECK(playing && fabs(bpm - 137.37f) < 0.01f, "joined at the leader's tempo");
        CHECK(worst < 0.25, "within %.3f ms of the leader (want under 0.25)", worst);
    }

    /* the clock out: this one leads at 120 BPM; each tick should go out when the heard sound passes
     * its 16th / 6 (stepped every millisecond, as the MIDI thread does) */
    mclock_reset(), playing = 0, bpm = 120.f, rate = 1.0;
    mclock_set_mode(MCLOCK_SEND);
    {
        playing = 1, pos = 0.0;
        uint64_t t = 100000000;
        int started = 0, n_ticks = 0;
        double worst = 0.0;
        long sent = -1;
        for (int i = 0; i < 20000; i++) { /* 20 s, 1 ms steps */
            run(1e-3);
            t += 1000;
            uint8_t out[16];
            int n = mclock_step(t, out, sizeof out);
            for (int j = 0; j < n; j++) {
                if (out[j] == 0xfa) started = 1, sent = (long)floor(heard() * 6.0);
                if (out[j] == 0xf8) {
                    sent++, n_ticks++;
                    /* when it went out against when the heard sound passed it */
                    double e = fabs(steps_ms(heard() - sent / 6.0, bpm));
                    if (i > 2000) worst = e > worst ? e : worst;
                }
            }
        }
        printf("clock out: START %d, %d ticks in 20 s (want %d), each within %.3f ms of the heard sound\n", started, n_ticks, 20 * 48, worst);
        CHECK(started && abs(n_ticks - 960) <= 2, "START, then 48 ticks a second (%d)", n_ticks);
        CHECK(worst < 1.5, "ticks with the heard sound: %.3f ms (want under 1.5, the step being 1 ms)", worst);
        uint8_t out[8], lp[LINKPOS_LEN];
        int nl = mclock_link_step(t + 30000, lp, sizeof lp);
        CHECK(nl == LINKPOS_LEN, "the link position goes out with it");
        playing = 0;
        int n = mclock_step(t + 1000, out, sizeof out);
        CHECK(n == 1 && out[0] == 0xfc, "stopping sends STOP");
    }
    /* SEND: an incoming clock isn't followed */
    mclock_reset(), playing = 0;
    mclock_realtime(0xfa, 200000000);
    for (int i = 1; i < 60; i++) mclock_realtime(0xf8, 200000000 + (uint64_t)i * 20833);
    CHECK(!mclock_following() && !playing, "SEND: a clock coming in is not followed");
    mclock_set_mode(MCLOCK_OFF);
    playing = 1;
    uint8_t o[4];
    CHECK(mclock_step(300000000, o, sizeof o) == 0, "OFF: nothing goes out");

    /* song position, then CONTINUE: stopped, with the clock running all the time (most gear), a
     * sequencer sends F2 (16th 32) and CONTINUE somewhere between two ticks; the first tick after it
     * marks 16th 32. The heard sound must be there from that tick on, wherever CONTINUE fell */
    mclock_set_mode(MCLOCK_AUTO);
    for (int c = 0; c < 4; c++) {
        static const double at[4] = {0.02, 0.35, 0.65, 0.98}; /* where between the ticks CONTINUE comes */
        mclock_reset(), playing = 0, bpm = 100.f, rate = 1.0;
        double tick_us = 60e6 / (120.0 * 24.0), worst = 0.0;
        uint64_t t0 = 400000000 + (uint64_t)c * 100000000, t = t0;
        double next = (double)t0;
        int sent = 0, k = -1, cont = 0;
        mclock_song_position(32);
        for (; t < t0 + 12000000; t += 50) { /* 2 s stopped, then 10 s on */
            run(5e-5);
            if (!cont && sent >= 96 && (double)t >= next - tick_us + at[c] * tick_us) {
                mclock_realtime(0xfb, t), cont = 1, k = 0;
            }
            if ((double)t >= next) {
                mclock_realtime(0xf8, t);
                sent++, next += tick_us;
                if (cont) {
                    double e = fabs(steps_ms(heard() - (32.0 + k / 6.0), 120.0));
                    worst = k > 0 && e > worst ? e : worst; /* from the tick after the one that places it */
                    k++;
                }
            }
        }
        printf("song position: CONTINUE %.2f of a tick after one, from 16th 32: plays %d, the tempo %.2f, within %.3f ms from the first tick on\n",
               at[c], playing, bpm, worst);
        CHECK(playing && worst < 0.5, "CONTINUE at %.2f of a tick: the heard sound at the 16th the ticks mark (%.3f ms)", at[c], worst);
    }
    {
        /* while playing: the app moves there at once */
        mclock_song_position(64);
        CHECK(fabs(pos - (64.0 + 0.02 * bpm / 15.0)) < 1e-5, "song position while playing moves the app there (%.4f)", pos);
        mclock_set_transport(0);
        mclock_song_position(0);
        CHECK(pos > 60.0, "the clock only: song position ignored");
        mclock_set_transport(1);
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
