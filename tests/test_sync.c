/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HI5S, the Wi-Fi sync: the packet is laid out as HI5, Jaffedrone, Columbia Street and RadioScan
 * expect (sync.h), an older unit's 32-byte packet is still understood, and the election picks the
 * leader the apps would. No sockets: the protocol core, driven directly. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/sync.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int mode = SYNC_AUTO, playing = 1;
static float bpm = 120.f;
static double pos = 3.0, rate = 1.0;
static int c_mode(void) { return mode; }
static float c_bpm(void) { return bpm; }
static void c_set_bpm(float b) { bpm = b; }
static double c_pos(void) { return pos; }
static void c_set_pos(double p) { pos = p; }
static void c_rate(double r) { rate = r; }
static int c_playing(void) { return playing; }
static void c_set_playing(int on) { playing = on; }
static const SyncClock CLK = {c_mode, c_bpm, c_set_bpm, c_pos, c_set_pos, c_rate, c_playing, c_set_playing};

static uint32_t rd32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }
static float rdf(const uint8_t *b) { float v; memcpy(&v, b, 4); return v; }
static double rdd(const uint8_t *b) { double v; memcpy(&v, b, 8); return v; }

/* a packet as an older unit sends it: the first 32 bytes */
static int old_packet(uint8_t *b, uint32_t id, float bpm_, double pos16, int play, int mode_, uint64_t t)
{
    memset(b, 0, 32);
    memcpy(b, "HI5S", 4);
    b[4] = 1, b[5] = (uint8_t)play, b[6] = (uint8_t)mode_, b[7] = 0;
    memcpy(b + 8, &id, 4), memcpy(b + 12, &bpm_, 4), memcpy(b + 16, &pos16, 8), memcpy(b + 24, &t, 8);
    return 32;
}

int main(void)
{
    uint8_t b[64];
    sync_core_init(&CLK, 500);
    int n = sync_make_packet(b, 1000000);
    CHECK(n == SYNC_PKT_LEN && n == 48, "48-byte packet (%d)", n);
    CHECK(!memcmp(b, "HI5S", 4) && b[4] == 1, "magic and version");
    CHECK(b[5] == 1 && b[6] == SYNC_AUTO, "playing and the mode at 5, 6");
    CHECK(rd32(b + 8) == 500, "the unit id at 8 (%u)", rd32(b + 8));
    CHECK(fabsf(rdf(b + 12) - 120.f) < 1e-4f, "bpm at 12 (%g)", rdf(b + 12));
    CHECK(rdd(b + 16) >= 3.0, "position at 16 (%g)", rdd(b + 16));

    /* an older unit, lower id: it leads, we follow its tempo */
    uint64_t t = 2000000;
    for (int k = 0; k < 20; k++, t += 100000) {
        n = old_packet(b, 100, 96.f, 8.0 + k * 0.08, 1, SYNC_AUTO, t);
        sync_handle_packet(b, n, t + 3000);
        sync_elect(t + 3000);
    }
    const SyncStatus *s = sync_status();
    CHECK(s->peers == 1, "the old unit is heard (%d)", s->peers);
    CHECK(s->following && !s->leading && s->leader_id == 100, "follows the lower id (leader %u)", s->leader_id);
    CHECK(fabsf(bpm - 96.f) < 0.01f, "takes its tempo (%g)", bpm);

    /* set to LEAD: leads, whatever the ids */
    mode = SYNC_LEAD;
    n = old_packet(b, 100, 96.f, 10.0, 1, SYNC_AUTO, t);
    sync_handle_packet(b, n, t + 3000);
    sync_elect(t + 3000);
    CHECK(s->leading && !s->following, "LEAD beats AUTO");
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
