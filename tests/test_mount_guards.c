/* Host-only test: mount functions must not touch foreign mounts. */
#define main ps4mount_console_main
#include "../src/main.c"
#undef main
#include <assert.h>

static int unmount_calls, mount_calls;
int statfs(const char *p, struct statfs *sf) {
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->f_mntonname, sizeof(sf->f_mntonname), "%s", p);
    snprintf(sf->f_fstypename, sizeof(sf->f_fstypename), "foreignfs");
    return 0;
}
int nmount(void *iov, unsigned int niov, int flags) {
    (void)iov; (void)niov; (void)flags;
    mount_calls++; return -1;
}
int unmount(const char *path, int flags) {
    (void)path; (void)flags;
    unmount_calls++; return -1;
}
int getfsstat(struct statfs *buf, long size, int flags) {
    (void)buf; (void)size; (void)flags;
    return -1;  /* unreadable running-state: must be fail-closed */
}
int main(void) {
    char template[] = "/tmp/ps4mount-mount-XXXXXX";
    char *root = mkdtemp(template); assert(root);
    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/CUSA01623", root);
    assert(mkdir(target, 0700) == 0);
    assert(top_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    assert(mount_dir("/mnt/usb0/ps4/CUSA01623", target, 1) == -1);
    assert(unmount_owned_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    assert(unmount_calls == 0 && mount_calls == 0);
    assert(title_running("CUSA01623") == 1);
    /* Phase 4B: refuse unmounts not attributable to our own source. */
    assert(probe_empty_dir(target) == 1);
    char inside[PATH_MAX];
    snprintf(inside, sizeof(inside), "%s/preserve.txt", target);
    FILE *f = fopen(inside, "wb"); assert(f);
    assert(fputs("do not obscure", f) >= 0); fclose(f);
    assert(probe_empty_dir(target) == 0);
    assert(unlink(inside) == 0);
    assert(probe_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", root);
    assert(probe_layer(missing, "/mnt/usb0/ps4/CUSA01623") == 0);
    assert(unmount_calls == 0 && mount_calls == 0);
    printf("PASS: Phase 4B marks foreign mountpoint and nonempty destination as unsafe\n");

    /* Two distinct attempts cannot reuse or guess a confirmation. */
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    char token[33];
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    assert(!probe_auth_consume("CUSA01623", "mount", token, NULL, NULL, NULL, NULL));
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    assert(probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    g_probe_auth.issued -= 121;
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(mount_calls == 0 && unmount_calls == 0);
    printf("PASS: Phase 4B rejects wrong-mode, expired, replayed, and invalid tokens\n");
    assert(rmdir(target) == 0);
    assert(rmdir(root) == 0);
    puts("PASS: foreign mounts left untouched and unreadable running-state blocks mutations");
    return 0;
}
