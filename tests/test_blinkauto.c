/* SPDX-License-Identifier: GPL-3.0-or-later */
/* blinkauto: the managed data channel. A session added on a socket pair against a plain blinkio session
 * standing in for the other Brick: the manager's audio reaches it with its heard position, the other's
 * reaches the manager, sending goes on while a second session comes and the first goes (unplugged), and a
 * gone Brick's session is dropped. */
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../src/blinkauto.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { int fd; } Sock;
static int s_send(void *ctx, const uint8_t *b, int n)
{
    Sock *k = ctx;
    while (n > 0) {
        ssize_t r = write(k->fd, b, (size_t)n);
        if (r <= 0) return -2;
        b += r, n -= (int)r;
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
static BlTransport sock(int fd)
{
    Sock *k = malloc(sizeof *k);
    k->fd = fd;
    return (BlTransport){s_send, s_recv, s_close, k};
}

static void nap_frames(int n) { struct timespec t = {0, (long)n * 1000000000L / 48000}; nanosleep(&t, NULL); }

int main(void)
{
    BlConfig mine = {.app = "JD", .name = "Jaffedrone", .ver = {1, 0, 0}, .rate = 48000, .nports = 1};
    mine.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_OUT, 0, 2, "OUT"};
    BlConfig theirs = {.app = "CS", .name = "Columbia Street", .ver = {0, 7, 0}, .rate = 48000, .nports = 1};
    theirs.ports[0] = (BlinkPort){BLINK_PORT_AUDIO_IN, 0, 2, "SAMPLE"};

    signal(SIGPIPE, SIG_IGN); /* a socket written after its other end has gone (the USB transports return an error) */
    bla_init(&mine);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { printf("no socketpair: skipped\n"); return 0; }
    CHECK(bla_add(sock(sv[0])), "a session added");
    BlSession *other = bl_start(&theirs, sock(sv[1]));
    for (int i = 0; i < 100 && bla_peers(NULL) < 1; i++) usleep(10000);
    BlinkHello h;
    CHECK(bla_peers(&h) == 1 && !strcmp(h.name, "Columbia Street"), "the other Brick heard: %s", h.name);

    /* the manager's audio thread: a 220 Hz sine with its position (64 frames a block at 170 BPM); the other
     * Brick takes it, with the position, and sends a 330 Hz sine back */
    enum { N = 64 };
    float out[2 * N], in[2 * N], back[2 * N];
    double spf = 170.0 / 15.0 / 48000.0, pos = 0.0, ph = 0.0, ph2 = 0.0;
    int heard_ok = 0, heard_n = 0, sound = 0, back_sound = 0;
    double max_step = 0.0, last = 0.0;
    for (int b = 0; b < 48000 * 3 / N; b++) { /* 3 s */
        for (int i = 0; i < N; i++) {
            out[2 * i] = out[2 * i + 1] = 0.4f * (float)sin(ph);
            ph += 2 * M_PI * 220 / 48000;
        }
        bla_send(0, out, N, pos);
        double hp = -1.0;
        if (bl_pull(other, 0, in, N, &hp)) {
            sound++;
            /* the sender's position, as sent with the frames: steady 16ths, moving at the tempo */
            if (hp >= 0.0) heard_n++, heard_ok += hp > 0.0 && hp <= pos + 1e-9;
            for (int i = 1; i < N; i++) max_step = fmax(max_step, fabs(in[2 * i] - (i ? in[2 * (i - 1)] : last)));
            last = in[2 * (N - 1)];
        }
        for (int i = 0; i < N; i++) {
            back[2 * i] = back[2 * i + 1] = 0.3f * (float)sin(ph2);
            ph2 += 2 * M_PI * 330 / 48000;
        }
        bl_send_audio(other, 0, back, N, -1);
        back_sound += bla_pull(0, in, N, NULL);
        pos += N * spf;
        nap_frames(N);
    }
    printf("blinkauto: sent 220 Hz with its position, heard %d of %d blocks, positions with them %d of %d, largest "
           "step %.4f (a clean sine at 0.4: 0.012); back from the other Brick %d blocks\n",
           sound, 48000 * 3 / N, heard_ok, heard_n, max_step, back_sound);
    CHECK(sound > 2000 && heard_n > 2000 && heard_ok == heard_n, "the other Brick takes the sound and its position");
    CHECK(max_step < 0.02, "with no clicks (largest step %.4f)", max_step);
    CHECK(back_sound > 2000, "and the manager takes the other Brick's (%d)", back_sound);

    /* a second Brick comes, the first goes (unplugged): sending goes on throughout, the gone one is dropped
     * (as the manager's own thread does; here bla_lead_start's thread isn't running, so by hand) */
    int sv2[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv2)) return 1;
    CHECK(bla_add(sock(sv2[0])), "a second session");
    BlSession *other2 = bl_start(&theirs, sock(sv2[1]));
    for (int i = 0; i < 100 && bla_peers(NULL) < 2; i++) usleep(10000);
    CHECK(bla_sessions() == 2 && bla_peers(NULL) == 2, "two Bricks (%d sessions, %d heard)", bla_sessions(), bla_peers(NULL));
    bl_stop(other); /* the first Brick unplugged */
    for (int b = 0; b < 48000 / N; b++) bla_send(0, out, N, pos), nap_frames(N);
    int got2 = 0;
    for (int b = 0; b < 200; b++) {
        bla_send(0, out, N, pos);
        got2 += bl_pull(other2, 0, in, N, NULL);
        nap_frames(N);
    }
    printf("blinkauto: a second Brick came and took %d blocks while the first went\n", got2);
    CHECK(got2 > 100, "the second Brick hears it (%d)", got2);
    bla_stop();
    CHECK(bla_sessions() == 0, "stopped: no sessions");
    bl_stop(other2);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
