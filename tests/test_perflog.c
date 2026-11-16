/* SPDX-License-Identifier: GPL-3.0-or-later */
/* perflog's dropout count against made-up callback times (built with fake_clock.h): no false
 * dropouts in a steady session, with a device clock a little slow or fast, or with callbacks in
 * bursts (as through PipeWire); a real stall still counted. The apps' copies count the same way. */
#include <stdio.h>
#include <stdint.h>

#include "../src/perflog.h"

unsigned long long fake_us = 1000000;
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* callbacks of 512 frames at 48 kHz for secs; the device's clock off by ppm; callbacks in bursts of
 * `burst`; one stall of stall_ms halfway. The dropouts counted */
static unsigned run(double ppm, int burst, double stall_ms, double secs)
{
    perf_audio_open(48000, 512, "test");
    double buf = 512e6 / 48000 * (1.0 + ppm * 1e-6), t = (double)fake_us + 1e5;
    long n = (long)(secs * 1e6 / buf);
    unsigned d0 = perf_dropouts();
    for (long i = 0; i < n; i++) {
        if (i == n / 2) t += stall_ms * 1000;
        fake_us = (unsigned long long)(t + (double)(i - i % burst) * buf + (i % burst) * 20.0);
        perf_audio_end(perf_audio_begin(512));
    }
    return perf_dropouts() - d0;
}

int main(void)
{
    perf_init("TEST", NULL);
    unsigned d;
    CHECK((d = run(0, 1, 0, 600)) == 0, "steady, 10 minutes: %u", d);
    CHECK((d = run(50, 1, 0, 600)) == 0, "a device clock 50 ppm slow: %u", d);
    CHECK((d = run(300, 1, 0, 600)) == 0, "300 ppm slow: %u", d);
    CHECK((d = run(-50, 1, 0, 600)) == 0, "50 ppm fast: %u", d);
    CHECK((d = run(0, 2, 0, 600)) == 0, "bursts of 2: %u", d);
    CHECK((d = run(0, 4, 0, 600)) == 0, "bursts of 4: %u", d);
    CHECK((d = run(50, 4, 0, 600)) == 0, "bursts of 4, 50 ppm slow: %u", d);
    CHECK((d = run(0, 1, 60, 60)) == 1, "a 60 ms stall: %u (want 1)", d);
    CHECK((d = run(0, 2, 60, 60)) == 1, "a 60 ms stall in bursts: %u (want 1)", d);
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
