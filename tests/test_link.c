/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The clock link's bytes are the same as the apps' (golden_link.h, from HI5's link.c; Conduit,
 * Columbia Street, Jaffedrone and RadioScan write the same), and link_find finds a linked Brick in a
 * made-up sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/link.h"
#include "golden_link.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) fputs(text, f), fclose(f);
}

int main(void)
{
    uint8_t b[512];
    int n = link_descriptors(b, sizeof b);
    CHECK(n == (int)sizeof GOLD_DESCRIPTORS && !memcmp(b, GOLD_DESCRIPTORS, sizeof GOLD_DESCRIPTORS), "descriptors as the apps' (%d bytes)", n);
    n = link_strings(b, sizeof b);
    CHECK(n == (int)sizeof GOLD_STRINGS && !memcmp(b, GOLD_STRINGS, sizeof GOLD_STRINGS), "strings as the apps' (%d bytes)", n);

    /* a sysfs tree: one linked Brick (vendor interface 42/4c, bulk OUT endpoint 01) and a keyboard */
    char root[] = "/tmp/brickio-linkXXXXXX";
    if (!mkdtemp(root)) { printf("no temp dir\n"); return 1; }
    char p[600];
    snprintf(p, sizeof p, "%s/1-1", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/1-1/busnum", root), put(p, "1\n");
    snprintf(p, sizeof p, "%s/1-1/devnum", root), put(p, "5\n");
    snprintf(p, sizeof p, "%s/1-1/idVendor", root), put(p, "1d6b\n");
    snprintf(p, sizeof p, "%s/1-1/idProduct", root), put(p, "0104\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/bInterfaceClass", root), put(p, "ff\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/bInterfaceSubClass", root), put(p, "42\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/bInterfaceProtocol", root), put(p, "4c\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/ep_01", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/ep_01/direction", root), put(p, "out\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/ep_01/bEndpointAddress", root), put(p, "01\n");
    snprintf(p, sizeof p, "%s/1-1/1-1:1.0/ep_01/type", root), put(p, "Bulk\n");
    snprintf(p, sizeof p, "%s/1-2", root), mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/1-2/idVendor", root), put(p, "046d\n");
    LinkDev ld[4];
    int found = link_find(root, ld, 4);
    printf("link_find: %d (bus %d dev %d ep %d)\n", found, found ? ld[0].bus : 0, found ? ld[0].dev : 0, found ? ld[0].ep : 0);
    CHECK(found == 1 && ld[0].bus == 1 && ld[0].dev == 5 && ld[0].ep == 1, "finds the linked Brick, not the keyboard");
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    if (system(cmd)) {}
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
