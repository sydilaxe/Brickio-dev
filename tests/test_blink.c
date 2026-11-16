/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel: the frames (blink.c), then two sessions (blinkio.c) talking over a
 * socket pair, as two Bricks would over USB: each with an audio clock of its own (100 ppm apart),
 * audio both ways on two ports, MIDI (SysEx too), HELLO, PING, and one side going away. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/blink.h"
#include "../src/blinkio.h"
#include "../src/link.h"
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ the frames */

static void test_frames(void)
{
    uint8_t buf[BLINK_MAX_TRANSFER], pay[BLINK_MAX_PAYLOAD];
    int n = 0;

    BlinkHello h = {.version = BLINK_VERSION, .app = "FX", .ver = {1, 2, 3}, .name = "TrimFX", .nports = 2};
    h.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_IN, 0, 2, "FX IN"};
    h.ports[1] = (BlinkPort){BLINK_PORT_AUDIO_OUT, 0, 2, "FX OUT"};
    int hn = blink_hello_pack(pay, sizeof pay, &h);
    n += blink_frame(buf + n, (int)sizeof buf - n, BLINK_HELLO, 0, 7, pay, hn);

    float s[64 * 2];
    for (int i = 0; i < 128; i++) s[i] = (float)i / 128 - 0.5f;
    int an = blink_audio_pack(pay, sizeof pay, 48000, 64, 2, 12.5, s);
    n += blink_frame(buf + n, (int)sizeof buf - n, BLINK_AUDIO, 1, 65535, pay, an);
    n += blink_frame(buf + n, (int)sizeof buf - n, 99, 0, 0, "new", 3); /* a type from a newer side */
    const uint8_t clk[] = {0xf8, 0xfa};
    n += blink_frame(buf + n, (int)sizeof buf - n, BLINK_MIDI, 2, 1, clk, 2);

    /* fed a byte at a time, then in odd pieces: the same frames */
    for (int step = 1; step <= 37; step += 36) {
        BlinkRx *rx = malloc(sizeof *rx);
        blink_rx_init(rx);
        int got = 0, ok_h = 0, ok_a = 0, ok_m = 0, other = 0;
        for (int i = 0; i < n; i += step) {
            blink_rx_feed(rx, buf + i, i + step > n ? n - i : step);
            BlinkFrame f;
            while (blink_rx_next(rx, &f)) {
                got++;
                BlinkHello g;
                BlinkAudio a;
                if (f.type == BLINK_HELLO && blink_hello_read(&f, &g))
                    ok_h = f.seq == 7 && !strcmp(g.app, "FX") && !strcmp(g.name, "TrimFX") && g.nports == 2 &&
                           g.ver[2] == 3 && !strcmp(g.ports[1].name, "FX OUT") && g.ports[1].kind == BLINK_PORT_AUDIO_OUT;
                else if (f.type == BLINK_AUDIO && blink_audio_read(&f, &a)) {
                    float d[128];
                    blink_samples(&a, d);
                    ok_a = f.port == 1 && f.seq == 65535 && a.rate == 48000 && a.frames == 64 && a.channels == 2 &&
                           a.heard == 12.5 && !memcmp(d, s, sizeof d);
                }
                else if (f.type == BLINK_MIDI)
                    ok_m = f.port == 2 && f.len == 2 && f.payload[0] == 0xf8 && f.payload[1] == 0xfa;
                else
                    other++;
            }
        }
        CHECK(got == 4 && ok_h && ok_a && ok_m && other == 1 && rx->bad == 0,
              "frames read back, %d bytes at a time (got %d, hello %d audio %d midi %d other %d bad %u)", step, got, ok_h,
              ok_a, ok_m, other, rx->bad);
        free(rx);
    }

    /* garbage in front and between: passed over, the frames still found */
    BlinkRx *rx = malloc(sizeof *rx);
    blink_rx_init(rx);
    uint8_t junk[] = {'B', 9, 9, 9, 0, 0, 0, 0, 'x', 'y', 'B'};
    blink_rx_feed(rx, junk, sizeof junk);
    blink_rx_feed(rx, buf, n);
    BlinkFrame f;
    int got = 0;
    while (blink_rx_next(rx, &f)) got++;
    CHECK(got == 4 && rx->bad == sizeof junk, "garbage passed over (%d frames, %u bytes passed over)", got, rx->bad);
    free(rx);

    /* padding: a transfer of a multiple of 64 bytes gets a NOP; the reader passes over it */
    uint8_t t[200];
    int m = blink_frame(t, sizeof t, BLINK_MIDI, 0, 0, pay, 56); /* 64 bytes */
    int p = blink_pad(t, sizeof t, m);
    CHECK(m == 64 && p == 72 && blink_pad(t, sizeof t, 72) == 72, "padding: 64 -> %d", p);

    CHECK(!blink_frame(t, sizeof t, BLINK_MIDI, 0, 0, pay, 300), "a frame that doesn't fit is refused");
    CHECK(!blink_audio_pack(pay, sizeof pay, 48000, 4096, 2, -1, NULL), "an AUDIO payload over the limit is refused");
    printf("frames: %d checks so far\n", checks);
}

/* ------------------------------------------------------------ two sessions */

typedef struct {
    int fd;
} Sock;

static int s_send(void *ctx, const uint8_t *b, int n)
{
    Sock *k = ctx;
    /* in two writes, a moment apart: the other side's reads see frames split */
    int half = n / 2 + 3 > n ? n : n / 2 + 3;
    for (int part = 0; part < 2; part++) {
        const uint8_t *p = part ? b + half : b;
        int len = part ? n - half : half;
        while (len > 0) {
            ssize_t r = write(k->fd, p, (size_t)len);
            if (r <= 0) return -2;
            p += r, len -= (int)r;
        }
        if (!part) usleep(200);
    }
    return 1;
}

static int s_recv(void *ctx, uint8_t *b, int max)
{
    Sock *k = ctx;
    ssize_t r = read(k->fd, b, (size_t)max);
    return r > 0 ? (int)r : r == 0 ? -2 : 0;
}

static void s_close(void *ctx)
{
    Sock *k = ctx;
    shutdown(k->fd, SHUT_RDWR);
    close(k->fd);
    free(k);
}

static BlTransport sock_transport(int fd)
{
    Sock *k = malloc(sizeof *k);
    k->fd = fd;
    return (BlTransport){s_send, s_recv, s_close, k};
}

static atomic_int midi_clocks, sysex_bytes, peer_ups, peer_downs;
static char peer_name[32];
static int peer_nports;

static void on_midi_b(int port, const uint8_t *b, int n, uint64_t t, void *ud)
{
    (void)t, (void)ud;
    for (int i = 0; i < n; i++) {
        if (port == 0 && b[i] == 0xf8) atomic_fetch_add(&midi_clocks, 1);
        if (port == 1) atomic_fetch_add(&sysex_bytes, 1);
    }
}

static void on_peer_b(const BlinkHello *h, int up, void *ud)
{
    (void)ud;
    if (up) {
        atomic_fetch_add(&peer_ups, 1);
        snprintf(peer_name, sizeof peer_name, "%s", h->name);
        peer_nports = h->nports;
    }
    else
        atomic_fetch_add(&peer_downs, 1);
}

/* a Brick's audio thread: 128 frames a period at its own clock */
typedef struct {
    BlSession *s;
    double rate; /* its true rate: 48000 x (1 + ppm) */
    int is_a;
    atomic_int run;
    /* A: the round trip of an impulse sent on port 1, echoed by B */
    long sent_at, frame;
    double rt_ms;
    int rt_count;
    /* B: the sine on port 0 as heard */
    double max_step, level;
    int sound_periods, periods;
} Brick;

static void *audio(void *arg)
{
    Brick *k = arg;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int N = 128;
    double period_ns = N * 1e9 / k->rate, acc = 0;
    float out0[256], out1[256], in0[256], in1[256];
    double phase = 0, prev = 0;
    int have_prev = 0;
    while (atomic_load(&k->run)) {
        acc += period_ns;
        long step = (long)acc;
        acc -= (double)step;
        t.tv_nsec += step;
        while (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        if (k->is_a) {
            for (int i = 0; i < N; i++) {
                float v = (float)(0.5 * sin(phase));
                phase += 2 * M_PI * 440 / 48000;
                out0[2 * i] = out0[2 * i + 1] = v;
                long fr = k->frame + i;
                float imp = fr % 24000 == 1000 ? 1.0f : 0.0f; /* an impulse every half second */
                out1[2 * i] = out1[2 * i + 1] = imp;
                if (imp > 0) k->sent_at = fr;
            }
            bl_send_audio(k->s, 0, out0, N, (double)k->frame / 6000);
            bl_send_audio(k->s, 1, out1, N, -1);
            bl_pull(k->s, 1, in1, N, NULL); /* the impulse, back from B */
            for (int i = 0; i < N; i++)
                if (in1[2 * i] > 0.5f && k->sent_at) {
                    k->rt_ms += (k->frame + i - k->sent_at) * 1000.0 / 48000, k->rt_count++;
                    k->sent_at = 0;
                }
        }
        else {
            k->periods++;
            int snd = bl_pull(k->s, 0, in0, N, NULL);
            if (snd) k->sound_periods++;
            if (snd && k->periods > 600) /* the last part: the cushion has settled */
                for (int i = 0; i < N; i++) {
                    double v = in0[2 * i];
                    if (have_prev && fabs(v - prev) > k->max_step) k->max_step = fabs(v - prev);
                    if (fabs(v) > k->level) k->level = fabs(v);
                    prev = v, have_prev = 1;
                }
            else have_prev = 0;
            bl_pull(k->s, 1, in1, N, NULL);
            bl_send_audio(k->s, 1, in1, N, -1); /* the impulse port, straight back */
        }
        k->frame += N;
    }
    return NULL;
}

static void test_sessions(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { CHECK(0, "socketpair"); return; }
    BlConfig ca = {.app = "H5", .name = "HI5", .ver = {1, 0, 0}, .rate = 48000, .nports = 1};
    ca.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_OUT, 0, 2, "HI5 OUT"};
    BlConfig cb = {.app = "FX", .name = "TrimFX", .ver = {0, 9, 0}, .rate = 48000, .nports = 2,
                   .on_midi = on_midi_b, .on_peer = on_peer_b};
    cb.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_IN, 0, 2, "FX IN"};
    cb.ports[1] = (BlinkPort){BLINK_PORT_MIDI_IN, 0, 0, "FX MIDI"};
    BlSession *a = bl_start(&ca, sock_transport(sv[0]));
    BlSession *b = bl_start(&cb, sock_transport(sv[1]));
    CHECK(a && b, "two sessions start");
    if (!a || !b) return;

    /* each Brick's own clock: B's 100 ppm slower than A's */
    Brick ka = {.s = a, .rate = 48000, .is_a = 1, .run = 1};
    Brick kb = {.s = b, .rate = 48000 * (1 - 100e-6), .is_a = 0, .run = 1};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, audio, &ka);
    pthread_create(&tb, NULL, audio, &kb);

    usleep(700000);
    BlinkHello ha;
    CHECK(bl_peer(a, &ha) && bl_peer(b, NULL) && !strcmp(ha.name, "TrimFX") && ha.nports == 2 &&
          !strcmp(ha.ports[1].name, "FX MIDI"), "each hears the other's HELLO (A sees \"%s\", %d ports)", ha.name, ha.nports);
    CHECK(atomic_load(&peer_ups) == 1 && !strcmp(peer_name, "HI5") && peer_nports == 1, "B's on_peer: up once, \"%s\"", peer_name);

    /* MIDI: clock on port 0, a 3000-byte SysEx on port 1 (split into frames on the way) */
    uint8_t clk = 0xf8;
    for (int i = 0; i < 24; i++) bl_send_midi(a, 0, &clk, 1);
    uint8_t *sx = malloc(3000);
    memset(sx, 0x11, 3000), sx[0] = 0xf0, sx[2999] = 0xf7;
    bl_send_midi(a, 1, sx, 3000);
    free(sx);
    bl_ping(a);

    usleep(3500000);
    atomic_store(&ka.run, 0), atomic_store(&kb.run, 0);
    pthread_join(ta, NULL), pthread_join(tb, NULL);

    BlStats sa, sb;
    bl_stats(a, 1, &sa);
    bl_stats(b, 0, &sb);
    double rt = ka.rt_count ? ka.rt_ms / ka.rt_count : -1;
    printf("A -> B: %u frames sent, %u received, %u lost, %u dropped; B's ring: cushion %u, drift %d ppm, %u underruns\n",
           sa.tx_frames, sb.rx_frames, sb.rx_lost, sa.tx_drops, sb.cushion, sb.drift_ppm, sb.underruns);
    printf("B's sine: level %.3f, largest step %.4f (a clean 440 Hz at 0.5: 0.029); sound %d of %d periods\n", kb.level,
           kb.max_step, kb.sound_periods, kb.periods);
    printf("the impulse's round trip, A -> B -> A through both rings: %.1f ms (%d times); PING %.2f ms\n", rt,
           ka.rt_count, sa.rtt_ms);
    CHECK(sb.rx_lost == 0 && sa.tx_drops == 0 && sb.rx_bad == 0, "nothing lost or dropped");
    CHECK(sb.rx_frames > 48000 * 3, "B received the audio (%u frames)", sb.rx_frames);
    CHECK(kb.level > 0.45 && kb.level < 0.55 && kb.max_step < 0.04, "B's sine is whole: no clicks (level %.3f, step %.4f)",
          kb.level, kb.max_step);
    CHECK(kb.sound_periods > kb.periods * 8 / 10, "B had sound most of the time");
    /* the drift figure takes the ring a minute to settle (tests/test_aring.c checks it over simulated
     * minutes); here: two clocks 100 ppm apart, and B's ring never ran dry */
    CHECK(sb.underruns == 0, "B's ring never ran dry with the clocks 100 ppm apart (%u underruns)", sb.underruns);
    CHECK(ka.rt_count >= 4 && rt > 0 && rt < 60, "the round trip is measured and short (%.1f ms)", rt);
    CHECK(atomic_load(&midi_clocks) == 24 && atomic_load(&sysex_bytes) == 3000, "MIDI arrived: %d clocks, %d SysEx bytes",
          atomic_load(&midi_clocks), atomic_load(&sysex_bytes));
    CHECK(sa.rtt_ms > 0, "PING answered (%.2f ms)", sa.rtt_ms);

    /* A goes (unplugged): B's transport ends, and B says so */
    bl_stop(a);
    for (int i = 0; i < 40 && bl_alive(b); i++) usleep(50000);
    CHECK(!bl_alive(b), "B knows A has gone");
    bl_stop(b);
    printf("sessions: done\n");
}

/* the other side stops answering but the transport stays (a leader app closed): the peer times out */
static void test_peer_timeout(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return;
    atomic_store(&peer_downs, 0);
    BlConfig cb = {.app = "FX", .name = "TrimFX", .on_peer = on_peer_b};
    BlSession *b = bl_start(&cb, sock_transport(sv[1]));
    /* a HELLO by hand, once */
    uint8_t pay[200], fr[256];
    BlinkHello h = {.version = 2, .app = "H5", .name = "HI5"};
    int n = blink_frame(fr, sizeof fr, BLINK_HELLO, 0, 0, pay, blink_hello_pack(pay, sizeof pay, &h));
    if (write(sv[0], fr, (size_t)n) != n) {}
    usleep(300000);
    int up = bl_peer(b, NULL);
    /* drain what B sends, so its writes don't block, while waiting out the timeout */
    uint8_t junk[4096];
    for (int i = 0; i < 20; i++) {
        usleep(100000);
        struct timeval tv = {0, 1000};
        setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        while (read(sv[0], junk, sizeof junk) > 0) {}
    }
    CHECK(up && !bl_peer(b, NULL) && atomic_load(&peer_downs) == 1, "a silent peer times out (up %d, then %d, downs %d)", up,
          bl_peer(b, NULL), atomic_load(&peer_downs));
    bl_stop(b);
    close(sv[0]);
}

/* ------------------------------------------------------------ the USB side */

static void put(const char *dir, const char *name, const char *text)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) fputs(text, f), fclose(f);
}

static void md(char *out, size_t n, const char *a, const char *b)
{
    snprintf(out, n, "%s/%s", a, b);
    mkdir(out, 0755);
}

static void test_usb(void)
{
    /* the data interface's descriptors: vendor, 'B' 'D', bulk OUT ep1 and IN ep2, full and high speed */
    uint8_t b[256];
    int n = link_data_descriptors(b, sizeof b);
    const struct usb_interface_descriptor *fs = (const void *)(b + sizeof(struct usb_functionfs_descs_head_v2) + 8);
    const struct usb_endpoint_descriptor_no_audio *o = (const void *)(fs + 1), *i = o + 1;
    const struct usb_interface_descriptor *hs = (const void *)(i + 1);
    const struct usb_endpoint_descriptor_no_audio *ho = (const void *)(hs + 1);
    CHECK(n == 12 + 8 + 2 * (9 + 7 + 7) && fs->bInterfaceClass == 0xff && fs->bInterfaceSubClass == BLINK_SUBCLASS &&
          fs->bInterfaceProtocol == BLINK_PROTOCOL && fs->bNumEndpoints == 2 && o->bEndpointAddress == 0x01 &&
          i->bEndpointAddress == 0x82 && o->bmAttributes == 2 && i->bmAttributes == 2 && o->wMaxPacketSize == 64 &&
          hs->bInterfaceProtocol == BLINK_PROTOCOL && ho->wMaxPacketSize == 512,
          "the data interface's descriptors (%d bytes)", n);
    CHECK(link_data_strings(b, sizeof b) > 0, "and its strings");

    /* a USB bus as sysfs shows it: a receiving Brick with the clock link (interface 0) and the data
     * channel (interface 1), an older one with the clock link alone, and a keyboard */
    char root[] = "/tmp/brickio-blinkXXXXXX", d[600], in[700], ep[800];
    if (!mkdtemp(root)) { CHECK(0, "temp dir"); return; }
    const char *devs[] = {"1-1", "1-2", "1-3"};
    for (int k = 0; k < 3; k++) {
        md(d, sizeof d, root, devs[k]);
        put(d, "busnum", "1\n");
        put(d, "devnum", k == 0 ? "5\n" : k == 1 ? "6\n" : "7\n");
        put(d, "idVendor", k == 2 ? "046d\n" : "1d6b\n");
        put(d, "idProduct", k == 2 ? "c31c\n" : "0104\n");
        if (k == 2) continue;
        char name[32];
        sprintf(name, "%s:1.0", devs[k]);
        md(in, sizeof in, d, name);
        put(in, "bInterfaceClass", "ff\n"), put(in, "bInterfaceSubClass", "42\n"), put(in, "bInterfaceProtocol", "4c\n");
        put(in, "bInterfaceNumber", "00\n");
        md(ep, sizeof ep, in, "ep_01");
        put(ep, "direction", "out\n"), put(ep, "type", "Bulk\n"), put(ep, "bEndpointAddress", "01\n");
        if (k == 1) continue;
        sprintf(name, "%s:1.1", devs[k]);
        md(in, sizeof in, d, name);
        put(in, "bInterfaceClass", "ff\n"), put(in, "bInterfaceSubClass", "42\n"), put(in, "bInterfaceProtocol", "44\n");
        put(in, "bInterfaceNumber", "01\n");
        md(ep, sizeof ep, in, "ep_02");
        put(ep, "direction", "out\n"), put(ep, "type", "Bulk\n"), put(ep, "bEndpointAddress", "02\n");
        md(ep, sizeof ep, in, "ep_83");
        put(ep, "direction", "in\n"), put(ep, "type", "Bulk\n"), put(ep, "bEndpointAddress", "83\n");
    }
    BlinkDev bd[4];
    int nb = bl_find(root, bd, 4);
    CHECK(nb == 1 && bd[0].dev == 5 && bd[0].iface == 1 && bd[0].ep_out == 0x02 && bd[0].ep_in == 0x83,
          "bl_find: the Brick with the data channel only (%d found)", nb);
    LinkDev ld[4];
    int nl = link_find(root, ld, 4);
    CHECK(nl == 2 && ld[0].ep == 1 && ld[1].ep == 1, "link_find still finds both clock links, on interface 0 (%d)", nl);
    char cmd[100];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
}

int main(void)
{
    test_frames();
    test_usb();
    test_sessions();
    test_peer_timeout();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
