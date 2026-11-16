/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The link's position message: the same bytes as the apps' (golden_linkpos.h, from HI5's
 * midi_link_pos), and read back. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/linkpos.h"
#include "golden_linkpos.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    for (int c = 0; c < 3; c++) {
        uint8_t b[LINKPOS_LEN];
        int n = linkpos_encode(b, sizeof b, GOLD_POS_HEARD[c], GOLD_POS_BPM[c], c & 1);
        CHECK(n == LINKPOS_LEN && !memcmp(b, GOLD_POS[c], LINKPOS_LEN), "case %d: the apps' bytes", c);
        double h, bpm;
        int pl;
        CHECK(linkpos_decode(b, n, &h, &bpm, &pl) && fabs(h - GOLD_POS_HEARD[c]) < 1.0 / 65536 && fabs(bpm - GOLD_POS_BPM[c]) < 0.001 && pl == (c & 1),
              "case %d: read back (%g %g %d)", c, h, bpm, pl);
        CHECK(linkpos_decode(b + 1, n - 2, &h, NULL, NULL), "case %d: without F0 / F7 (as the apps keep it)", c);
    }
    static const uint8_t other[] = {0xf0, 0x7d, 'X', 'L', 1, 2, 3, 4, 5, 6, 7, 8, 1, 0xf7};
    CHECK(!linkpos_decode(other, sizeof other, NULL, NULL, NULL), "another 7D message is not taken for it");
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
