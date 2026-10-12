/* Host regression tests: param.sfo must not read past untrusted PKG bytes. */
#include "../src/pkg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void le32_put(unsigned char *p, unsigned n) {
    p[0] = n & 255; p[1] = (n >> 8) & 255;
    p[2] = (n >> 16) & 255; p[3] = (n >> 24) & 255;
}
static void le16_put(unsigned char *p, unsigned n) { p[0] = n & 255; p[1] = (n >> 8) & 255; }
int main(void) {
    unsigned char d[120] = {0};
    sfo_t s;
    le32_put(d, 0x46535000);  /* PSF magic */
    le32_put(d + 8, 52);      /* keys start */
    le32_put(d + 12, 72);     /* values start */
    le32_put(d + 16, 2);      /* entries */
    le16_put(d + 20, 0); le16_put(d + 22, 0x0204);
    le32_put(d + 24, 3); le32_put(d + 32, 0);
    le16_put(d + 36, 9); le16_put(d + 38, 0x0204);
    le32_put(d + 40, 6); le32_put(d + 48, 3);
    memcpy(d + 52, "CATEGORY\0APP_VER\0", 17);
    memcpy(d + 72, "gp\0" "01.10\0", 9);
    assert(!sfo_parse(d, sizeof(d), &s));
    assert(sfo_get(&s, "CATEGORY") && !strcmp(sfo_get(&s, "CATEGORY")->sval, "gp"));
    assert(sfo_get(&s, "APP_VER") && !strcmp(sfo_get(&s, "APP_VER")->sval, "01.10"));
    assert(sfo_parse(d, 20, &s));
    le32_put(d + 8, 119); assert(sfo_parse(d, sizeof(d), &s));
    le32_put(d + 8, 52);
    le32_put(d + 48, 40000000); assert(sfo_parse(d, sizeof(d), &s));
    le32_put(d + 48, 3);
    le32_put(d + 16, 40000000); assert(sfo_parse(d, sizeof(d), &s));
    le32_put(d + 16, 2);
    memset(d + 52, 'x', 68); assert(sfo_parse(d, sizeof(d), &s));
    printf("PASS: SFO valid values, truncated records, keys and out-of-range offsets\n");
    return 0;
}
