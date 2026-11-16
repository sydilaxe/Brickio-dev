/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel, tested on two Bricks with no app running (as root, over SSH).
 *
 *   receiving Brick (its bottom port to the hub):   ./blink-test follow
 *   primary Brick (a hub in its top port):          ./blink-test lead [seconds] [frames]
 *
 * The follower sets its bottom port up as the clock link plus the data channel and sends back whatever
 * audio it receives (a loop through it). The leader finds it, sends a quiet 440 Hz tone with a click
 * every second and MIDI clock at 120 BPM, and reports once a second: the click's round trip (out,
 * through both Bricks' rings, back), audio lost or dropped, the rings running dry, the drift between
 * the two Bricks' clocks, the link's own round trip (PING), and on the follower the MIDI clock's rate.
 * Each Brick's audio runs on its own clock, as it would with an app, 128 frames a period (or [frames]).
 *
 * Built for the Brick by `make tools` (build/aarch64/blink-test). */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/blinkio.h"
#include "../src/link.h"

static atomic_int run = 1;
static void on_int(int s) { (void)s; atomic_store(&run, 0); }

static BlSession *ses;
static int lead, frames_per = 128;
static atomic_int clocks;            /* the follower: MIDI clocks this second */
static atomic_int rt_n;
static _Atomic double rt_sum, rt_max;

static void on_midi(int port, const uint8_t *b, int n, uint64_t t, void *ud)
{
    (void)port, (void)t, (void)ud;
    for (int i = 0; i < n; i++)
        if (b[i] == 0xf8) atomic_fetch_add(&clocks, 1);
}

static void on_peer(const BlinkHello *h, int up, void *ud)
{
    (void)ud;
    if (up) {
        printf("** the other Brick: %s (%.2s) %d.%d.%d, %d ports:", h->name, h->app, h->ver[0], h->ver[1], h->ver[2], h->nports);
        for (int i = 0; i < h->nports; i++) printf(" [%s]", h->ports[i].name);
        printf("\n");
    }
    else
        printf("** the other Brick has gone quiet\n");
    fflush(stdout);
}

static void *audio(void *arg)
{
    (void)arg;
    struct sched_param sp = {.sched_priority = 80};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
        printf("(the audio thread isn't real-time: run as root)\n");
    const int N = frames_per;
    float *out = calloc((size_t)N * 2, sizeof(float)), *in = calloc((size_t)N * 2, sizeof(float));
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double acc = 0, phase = 0;
    long frame = 0, sent_at = -1;
    while (atomic_load(&run)) {
        acc += N * 1e9 / 48000;
        long step = (long)acc;
        acc -= (double)step;
        t.tv_nsec += step;
        while (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        int snd = bl_pull(ses, 0, in, N, NULL);
        if (lead) {
            for (int i = 0; i < N; i++) {
                long f = frame + i;
                float v = (float)(0.2 * sin(phase));
                phase += 2 * M_PI * 440 / 48000;
                if (f % 48000 == 0) v = 1.0f, sent_at = f;
                out[2 * i] = out[2 * i + 1] = v;
                if (snd && sent_at >= 0 && in[2 * i] > 0.7f) {
                    double ms = (f - sent_at) * 1000.0 / 48000;
                    atomic_store(&rt_sum, atomic_load(&rt_sum) + ms);
                    if (ms > atomic_load(&rt_max)) atomic_store(&rt_max, ms);
                    atomic_fetch_add(&rt_n, 1);
                    sent_at = -1;
                }
            }
            bl_send_audio(ses, 0, out, N, -1);
        }
        else
            bl_send_audio(ses, 0, in, N, -1); /* straight back */
        frame += N;
    }
    free(out), free(in);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "lead") && strcmp(argv[1], "follow"))) {
        fprintf(stderr, "usage: blink-test follow | lead [seconds] [frames]\n");
        return 2;
    }
    lead = !strcmp(argv[1], "lead");
    int secs = argc > 2 ? atoi(argv[2]) : 30;
    if (argc > 3) frames_per = atoi(argv[3]);
    if (frames_per < 16 || frames_per > 1024) frames_per = 128;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_int), signal(SIGTERM, on_int);

    BlTransport tr;
    if (lead) {
        BlinkDev d[8];
        int n = 0;
        for (int i = 0; i < 20 && !(n = bl_find("/sys/bus/usb/devices", d, 8)) && atomic_load(&run); i++) {
            if (!i) printf("looking for a Brick with the data channel on the USB bus...\n");
            sleep(1);
        }
        if (!n) { printf("none found: is ./blink-test follow running on the other Brick, its bottom port in the hub?\n"); return 1; }
        printf("found %d; using bus %d device %d (interface %d, endpoints %02x out %02x in)\n", n, d[0].bus, d[0].dev,
               d[0].iface, d[0].ep_out, d[0].ep_in);
        if (!bl_usb_leader(&d[0], &tr)) { perror("can't open it"); return 1; }
    }
    else {
        link_follow_data(1);
        if (!link_follow_start("BLT", NULL, NULL, NULL)) { printf("the bottom port couldn't be set up (no app open? root?)\n"); return 1; }
        if (!bl_usb_follower(&tr)) { printf("the clock link is up, but the data channel isn't (see above)\n"); link_follow_stop(); return 1; }
        printf("the bottom port is the clock link and the data channel: waiting for the leader...\n");
        secs = 0; /* until stopped */
    }

    BlConfig c = {.app = "BT", .name = lead ? "blink-test lead" : "blink-test follow", .ver = {0, 2, 0}, .rate = 48000,
                  .nports = 2, .on_midi = on_midi, .on_peer = on_peer};
    c.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_OUT, 0, 2, "LOOP"};
    if (lead) snprintf(c.ports[0].name, sizeof c.ports[0].name, "TONE");
    c.ports[1] = (BlinkPort){BLINK_PORT_AUDIO_IN, 0, 2, "IN"};
    ses = bl_start(&c, tr);
    if (!ses) { printf("the session didn't start\n"); return 1; }
    pthread_t th;
    pthread_create(&th, NULL, audio, NULL);

    uint64_t clock_n = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int s = 1; atomic_load(&run) && (!secs || s <= secs); s++) {
        /* MIDI clock at 120 BPM (48 a second) from the leader, on time-table */
        for (int k = 0; k < 48 && atomic_load(&run); k++) {
            clock_n++;
            long ns = (long)(clock_n * (1000000000L / 48));
            t.tv_sec = t0.tv_sec + ns / 1000000000L, t.tv_nsec = t0.tv_nsec + ns % 1000000000L;
            if (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
            if (lead) { uint8_t f8 = 0xf8; bl_send_midi(ses, 0, &f8, 1); }
        }
        if (lead) bl_ping(ses);
        BlStats st;
        bl_stats(ses, 0, &st);
        int rn = atomic_exchange(&rt_n, 0);
        double rs = atomic_exchange(&rt_sum, 0.0), rm = atomic_exchange(&rt_max, 0.0);
        printf("%4ds %s | sent %u (dropped %u) got %u (lost %u, bad bytes %u) | ring %u fr, dry %u, skips %u, drift %+d ppm",
               s, st.peer ? "linked" : "alone ", st.tx_frames, st.tx_drops, st.rx_frames, st.rx_lost, st.rx_bad,
               st.cushion, st.underruns, st.skips, st.drift_ppm);
        if (lead)
            printf(" | click round trip %s%.1f ms (max %.1f) | link %.2f ms\n", rn ? "" : "-", rn ? rs / rn : 0.0, rm, st.rtt_ms);
        else
            printf(" | MIDI clock %d/s (want 48)\n", atomic_exchange(&clocks, 0));
        if (!bl_alive(ses)) { printf("the other Brick has gone (unplugged?)\n"); break; }
    }
    atomic_store(&run, 0);
    pthread_join(th, NULL);
    BlStats st;
    bl_stats(ses, 0, &st);
    printf("end: sent %u, got %u, lost %u, dropped %u, dry %u, transfers %u out %u in, send fails %u\n", st.tx_frames,
           st.rx_frames, st.rx_lost, st.tx_drops, st.underruns, st.transfers_out, st.transfers_in, st.send_fails);
    bl_stop(ses);
    if (!lead) link_follow_stop();
    return 0;
}
