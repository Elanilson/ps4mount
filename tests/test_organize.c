/* Phase 3B host-only tests: always use disposable temp directory. */
#define _GNU_SOURCE
#include "../src/organize.c"
#include <assert.h>
#include <dirent.h>

static char home[PATH_MAX];
static void path(char *out, const char *suffix) {
    assert(snprintf(out, PATH_MAX, "%s%s", home, suffix) < PATH_MAX);
}
static void put(const char *p, const char *data) {
    FILE *f = fopen(p, "wb"); assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data)); assert(!fclose(f));
}
static void fixture(organization_t *r, const char *tid) {
    memset(r, 0, sizeof(*r));
    snprintf(r->root, sizeof(r->root), "%s", home);
    snprintf(r->tid, sizeof(r->tid), "%s", tid);
    path(r->app_source, "/old-game/app_old.pkg");
    path(r->patch_source, "/old-game/update_old.pkg");
    snprintf(r->app_destination, sizeof(r->app_destination), "%s/ps4/%s - Demo/app.pkg", home, tid);
    snprintf(r->patch_destination, sizeof(r->patch_destination), "%s/ps4/%s - Demo/patch.pkg", home, tid);
}
static void reset(const char *sfx) {
    /* isolate each scenario in its own fresh root */
    snprintf(home, sizeof(home), "/tmp/ps4mount-org-test-%ld-%s", (long)getpid(), sfx);
    assert(!mkdir(home, 0700));
    char p[PATH_MAX]; path(p, "/old-game"); assert(!mkdir(p, 0700));
    path(p, "/old-game/app_old.pkg"); put(p, "gd CUSA01623 some long base file");
    path(p, "/old-game/update_old.pkg"); put(p, "gp CUSA01623 v1.01 patch file");
}
static void mkdir_destination(void) {
    char p[PATH_MAX]; path(p, "/ps4"); assert(!mkdir(p, 0700));
    path(p, "/ps4/CUSA01623 - Demo"); assert(!mkdir(p, 0700));
}
static void create_journal(const organization_t *r) {
    org_journal_t *j = calloc(1, sizeof(*j)); assert(j);
    memcpy(j->magic, ORG_MAGIC, sizeof(j->magic));
    snprintf(j->root, sizeof(j->root), "%s", r->root);
    snprintf(j->tid, sizeof(j->tid), "%s", r->tid);
    const char *source[] = {r->app_source, r->patch_source}, *dest[] = {r->app_destination, r->patch_destination};
    for (int i = 0; i < 2; i++) {
        j->file[i].present = 1;
        snprintf(j->file[i].source, PATH_MAX, "%s", source[i]);
        snprintf(j->file[i].destination, PATH_MAX, "%s", dest[i]);
        assert(!file_signature(source[i], &j->file[i].size, &j->file[i].signature));
    }
    assert(!write_journal(r->root, j));
    free(j);
}
static void rm_tree(const char *p) {
    DIR *d = opendir(p);
    if (!d) { assert(!unlink(p)); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_MAX]; snprintf(child, sizeof(child), "%s/%s", p, e->d_name);
        struct stat st; assert(!lstat(child, &st));
        if (S_ISDIR(st.st_mode)) rm_tree(child);
        else assert(!unlink(child));
    }
    closedir(d); assert(!rmdir(p));
}
int main(void) {
    char msg[256], p[PATH_MAX]; organization_t o;
    reset("success"); fixture(&o, "CUSA01623");
    assert(!org_pending(home));
    assert(!org_apply(&o, msg, sizeof(msg)));
    assert(!org_pending(home));
    assert(exists_any(o.app_source) == 0 && exists_any(o.patch_source) == 0);
    assert(exists_any(o.app_destination) == 1 && exists_any(o.patch_destination) == 1);
    printf("PASS: one-title base + patch moved in place, journal cleared\n");
    rm_tree(home);

    reset("overwrite"); fixture(&o, "CUSA01623"); mkdir_destination();
    put(o.app_destination, "DO NOT OVERWRITE");
    assert(org_apply(&o, msg, sizeof(msg)) != 0);
    assert(exists_any(o.app_source) == 1 && exists_any(o.patch_source) == 1);
    assert(!org_pending(home));
    printf("PASS: existing destination prevents any PKG rename\n");
    rm_tree(home);

    reset("link"); fixture(&o, "CUSA01623");
    path(p, "/ps4"); assert(!symlink("old-game", p));
    assert(org_apply(&o, msg, sizeof(msg)) != 0);
    assert(exists_any(o.app_source) == 1);
    printf("PASS: symlink destination rejected\n");
    rm_tree(home);

    reset("recover"); fixture(&o, "CUSA01623"); mkdir_destination();
    create_journal(&o);
    assert(!rename(o.app_source, o.app_destination)); /* simulate sudden power loss */
    assert(org_pending(home) == 1);
    assert(org_inspect(home, msg, sizeof(msg)) == 1);
    assert(org_apply(&o, msg, sizeof(msg)) != 0); /* no other writes allowed */
    assert(!org_rollback(home, msg, sizeof(msg)));
    assert(!org_pending(home));
    assert(exists_any(o.app_source) == 1 && exists_any(o.patch_source) == 1);
    assert(exists_any(o.app_destination) == 0 && exists_any(o.patch_destination) == 0);
    printf("PASS: simulated interrupt allows EXPLICIT recovery only\n");
    rm_tree(home);

    reset("bothmoved"); fixture(&o, "CUSA01623"); mkdir_destination();
    create_journal(&o);
    assert(!rename(o.app_source, o.app_destination));
    assert(!rename(o.patch_source, o.patch_destination));
    assert(!org_rollback(home, msg, sizeof(msg)));
    assert(exists_any(o.app_source) == 1 && exists_any(o.patch_source) == 1);
    printf("PASS: recovery reverts both packages after simulated power loss\n");
    rm_tree(home);

    reset("collision"); fixture(&o, "CUSA01623"); mkdir_destination();
    create_journal(&o);
    put(o.app_destination, "foreign data");
    assert(org_rollback(home, msg, sizeof(msg)) != 0);
    assert(org_pending(home) == 1);
    assert(exists_any(o.app_source) == 1);
    printf("PASS: rollback refuses existing/changed destination and preserves journal\n");
    rm_tree(home);

    reset("corrupt"); fixture(&o, "CUSA01623"); mkdir_destination();
    path(p, JOURNAL_FILE); put(p, "invalid journal");
    assert(org_inspect(home, msg, sizeof(msg)) != 0);
    assert(org_apply(&o, msg, sizeof(msg)) != 0);
    assert(org_rollback(home, msg, sizeof(msg)) != 0);
    assert(exists_any(o.app_source) == 1);
    printf("PASS: malformed journal fails closed and prevents any write\n");
    rm_tree(home);

    reset("traversal"); fixture(&o, "CUSA01623");
    path(o.app_destination, "/ps4/../old-game/overwrite.pkg");
    assert(org_apply(&o, msg, sizeof(msg)) != 0);
    assert(exists_any(o.app_source) == 1);
    printf("PASS: path traversal is rejected\n");
    rm_tree(home);

    reset("baseonly"); fixture(&o, "CUSA01623");
    o.patch_source[0] = 0;
    assert(!org_apply(&o, msg, sizeof(msg)));
    assert(exists_any(o.app_destination) == 1);
    assert(exists_any(o.patch_destination) == 0);
    printf("PASS: game without approved update moves base only\n");
    rm_tree(home);

    puts("PASS: all Phase 3B USB transaction tests passed");
    return 0;
}
