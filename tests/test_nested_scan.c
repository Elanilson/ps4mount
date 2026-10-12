/* Host-only read-only scanner and isolated same-volume preparation tests.
 * Simulated PKGs only. This must NEVER operate on a real PS5 mount. */
#define main ps4mount_console_main
#include "../src/main.c"
#undef main
#include <assert.h>

/* The isolated scanner host fixture has no PS5 app.db. */
int db_registered_status(const char *tid) { (void)tid; return 0; }

/* Minimal host buffer helpers needed to exercise the authenticated preview. */
void buf_put(buf_t *b, const void *data, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t want = b->len + len + 1;
        b->data = realloc(b->data, want);
        assert(b->data);
        b->cap = want;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
}
void buf_printf(buf_t *b, const char *fmt, ...) {
    char tmp[4096];
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    if (len > 0) buf_put(b, tmp, (size_t)len < sizeof(tmp) ? (size_t)len : sizeof(tmp) - 1);
}
void buf_free(buf_t *b) { free(b->data); memset(b, 0, sizeof(*b)); }

static char test_root[PATH_MAX];

/* Fake pkg files have "gd CUSA01623" or "gp CUSA01623" or "ac ...". */
int pkg_open(pkg_t *pkg, const char *path) {
    memset(pkg, 0, sizeof(*pkg));
    pkg->fd = open(path, O_RDONLY);
    if (pkg->fd < 0) return -1;
    char category[4] = "", id[12] = "";
    FILE *f = fdopen(dup(pkg->fd), "r");
    char version[16] = "01.03";
    int count = f ? fscanf(f, "%3s %11s %15s", category, id, version) : 0;
    int ok = count >= 2;
    if (f) fclose(f);
    if (!ok) { close(pkg->fd); pkg->fd = -1; return -1; }
    snprintf((char *)pkg->header, sizeof(pkg->header), "%.2s %s", category, version);
    snprintf(pkg->title_id, sizeof(pkg->title_id), "%s", id);
    snprintf(pkg->content_id, sizeof(pkg->content_id), "UP0000-%.9s_00-TESTGAME00000000", id);
    /* optional trailing mismatched content ID marker for incompatibility test */
    if (strstr(path, "Foreign"))
        snprintf(pkg->content_id, sizeof(pkg->content_id), "UP0000-%.9s_00-FOREIGN000000000", id);
    pkg->pkg_size = 123;
    return 0;
}
void pkg_close(pkg_t *pkg) { if (pkg->fd >= 0) close(pkg->fd); pkg->fd = -1; }
int pkg_fake_key(pkg_t *pkg) { (void)pkg; return 1; }
uint8_t *pkg_entry(pkg_t *pkg, uint32_t id, size_t *len) {
    if (id != 0x1000) return NULL;
    *len = strlen((char *)pkg->header) + 1;
    uint8_t *data = malloc(*len);
    memcpy(data, pkg->header, *len);
    return data;
}
int sfo_parse(const uint8_t *data, size_t len, sfo_t *sfo) {
    (void)len;
    memset(sfo, 0, sizeof(*sfo));
    sfo->count = 3;
    strcpy(sfo->items[0].key, "CATEGORY");
    snprintf(sfo->items[0].sval, sizeof(sfo->items[0].sval), "%.2s", data);
    strcpy(sfo->items[1].key, "TITLE");
    strcpy(sfo->items[1].sval, "Game: Test / PS4");
    strcpy(sfo->items[2].key, "APP_VER");
    strcpy(sfo->items[2].sval, (const char *)data + 3);
    return 0;
}
const sfo_item_t *sfo_get(const sfo_t *sfo, const char *key) {
    for (int i = 0; i < sfo->count; i++) if (!strcmp(sfo->items[i].key, key)) return &sfo->items[i];
    return NULL;
}
int statfs(const char *p, struct statfs *sf) {
    if (strncmp(p, test_root, strlen(test_root))) return -1;
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->f_mntonname, sizeof(sf->f_mntonname), "%s", p);
    return 0;
}

static void cleanup_fixture(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode)) cleanup_fixture(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void write_dummy(const char *p, const char *txt) {
    FILE *f = fopen(p, "w"); assert(f);
    assert(fputs(txt, f) >= 0); fclose(f);
}

int main(void) {
    char tmp[] = "/tmp/ps4mount-test-XXXXXX";
    char *root = mkdtemp(tmp); assert(root);
    snprintf(test_root, sizeof(test_root), "%s", root);
    char game[PATH_MAX], updates[PATH_MAX], dlcs[PATH_MAX], other[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(game, sizeof(game), "%s/My Game", root); assert(!mkdir(game, 0700));
    snprintf(updates, sizeof(updates), "%s/Updates", game); assert(!mkdir(updates, 0700));
    snprintf(dlcs, sizeof(dlcs), "%s/DLC", game); assert(!mkdir(dlcs, 0700));
    snprintf(other, sizeof(other), "%s/Another Game", root); assert(!mkdir(other, 0700));
    snprintf(p, sizeof(p), "%s/Original Game.pkg", game); write_dummy(p, "gd CUSA01623");
    snprintf(p, sizeof(p), "%s/Update Old.pkg", updates); write_dummy(p, "gp CUSA01623 01.09");
    snprintf(p, sizeof(p), "%s/Update Version.pkg", updates); write_dummy(p, "gp CUSA01623 01.10");
    snprintf(p, sizeof(p), "%s/Update Much Older.pkg", updates); write_dummy(p, "gp CUSA01623 01.02");
    snprintf(p, sizeof(p), "%s/Foreign Update.pkg", updates); write_dummy(p, "gp CUSA01623 99.99");
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); write_dummy(p, "ac CUSA01623");
    snprintf(p, sizeof(p), "%s/Another Game.pkg", other); write_dummy(p, "gd CUSA01234");
    snprintf(p, sizeof(p), "%s/Orphan Update.pkg", updates); write_dummy(p, "gp CUSA09999");

    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    title_t *t = catalog_find("CUSA01623"), *t2 = catalog_find("CUSA01234");
    assert(t && t2 && t->app_path[0] && t->patch_path[0]);
    assert(strstr(t->app_path, "Original Game.pkg"));
    assert(strstr(t->patch_path, "Update Version.pkg"));
    assert(!strcmp(t->patch_version, "01.10"));
    assert(t->update_count == 4 && t->incompatible_updates == 1 && !t->patch_conflict);
    assert(ps4mount_compare_app_ver("01.10", "01.09") > 0);
    assert(ps4mount_compare_app_ver("01.10", "01.10") == 0);
    assert(ps4mount_compare_app_ver("01.10", "02.00") < 0);
    int vparts[4]; assert(ps4mount_parse_app_ver("01.X", vparts) != 0);
    printf("PASS: most recent compatible APP_VER wins, unrelated content id ignored\n");
    assert(!t->conflict && !t->patch_conflict);
    assert(!t2->patch_path[0]);
    snprintf(p, sizeof(p), "%s/app.pkg", game); assert(!exists(p));
    snprintf(p, sizeof(p), "%s/Original Game.pkg", game); assert(exists(p));
    snprintf(p, sizeof(p), "%s/Update Version.pkg", updates); assert(exists(p));
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); assert(exists(p));
    printf("PASS: recursive scanning is read-only and joins update by CUSA across folders\n");

    /* Phase 4A: read-only preflight gating is fail-closed for every bit. */
    assert(phase4_ready(PH4_REQUIRED));
    for (unsigned int bit = 1; bit <= PH4_DB_ABSENT; bit <<= 1) {
        assert(!phase4_ready(PH4_REQUIRED & ~bit));
        assert(strcmp(phase4_first_blocker(PH4_REQUIRED & ~bit),
                      phase4_first_blocker(PH4_REQUIRED)) != 0);
    }
    assert(!phase4_ready(PH4_CATALOG | PH4_CONFLICT_FREE));
    assert(strstr(phase4_first_blocker(PH4_REQUIRED & ~PH4_ORGANIZED), "not yet"));
    buf_t launch_json = {0};
    assert(app_launch_check_json("CUSA99999", &launch_json) == 0);
    assert(strstr((char *)launch_json.data, "\"preflight_passed\":false"));
    assert(strstr((char *)launch_json.data, "\"launchable\":false"));
    assert(strstr((char *)launch_json.data, "\"registration_enabled\":false"));
    buf_free(&launch_json);
    printf("PASS: phase 4A read-only single-title preflight requires EVERY check; missing title blocked\n");

    /* Test registration is hard-disabled without touching the file tree. */
    char msg[256];
    assert(app_add("CUSA01623", msg, sizeof(msg)) != 0);
    snprintf(p, sizeof(p), "%s/Original Game.pkg", game); assert(exists(p));
    printf("PASS: all console registration requests are rejected in preview mode\n");

    /* Phase 3A preflight: generates a plan WITHOUT making directories or
     * renaming files; checks conflicts and refuses overwrite/link destinations. */
    title_plan_t preview;
    char why[256];
    assert(plan_title(t, &preview, why, sizeof(why)) == 0);
    assert(preview.create_library && preview.create_folder);
    assert(preview.app_move && preview.patch_move);
    assert(strstr(preview.app_destination, "/ps4/CUSA01623 - Game_ Test _ PS4/app.pkg"));
    assert(strstr(preview.patch_destination, "/ps4/CUSA01623 - Game_ Test _ PS4/patch.pkg"));
    snprintf(p, sizeof(p), "%s/ps4", root); assert(!exists(p));
    assert(!exists(preview.app_destination) && !exists(preview.patch_destination));
    printf("PASS: phase 3A plan previews base/update paths without writing\n");

    /* Host test uses /tmp, so only test random nonce authorization here:
       HTTP preview rejects non-/mnt/usb roots on real PS5 by design. */
    auth_clear();
    assert(auth_nonce() == 0);
    g_move_auth.issued = time(NULL);
    snprintf(g_move_auth.tid, sizeof(g_move_auth.tid), "%s", "CUSA01623");
    char auth_token[33]; snprintf(auth_token, sizeof(auth_token), "%s", g_move_auth.nonce);
    assert(!auth_matches("00000000000000000000000000000000", 0));
    assert(!auth_matches(auth_token, 1));
    assert(app_organize("CUSA01234", auth_token, msg, sizeof(msg)) != 0);
    g_move_auth.issued -= 121;
    assert(!auth_matches(auth_token, 0));
    assert(app_organize("CUSA01623", auth_token, msg, sizeof(msg)) != 0);
    auth_clear();
    assert(exists(t->app_path) && exists(t->patch_path));
    printf("PASS: wrong-title and expired organization confirmation tokens rejected\n");

    /* A pre-existing destination blocks a plan: NEVER overwrite. */
    snprintf(p, sizeof(p), "%s/ps4", root); assert(!mkdir(p, 0700));
    assert(!mkdir(preview.destination, 0700));
    write_dummy(preview.app_destination, "someone else's file");
    assert(plan_title(t, &preview, why, sizeof(why)) != 0);
    assert(!unlink(preview.app_destination));
    assert(!rmdir(preview.destination));
    printf("PASS: existing destination prevents any organization plan\n");

    /* A symlink pointing outside the drive never gets traversed. */
    snprintf(p, sizeof(p), "%s/ps4", root);
    assert(!rmdir(p));
    assert(!symlink(other, p));
    assert(plan_title(t, &preview, why, sizeof(why)) != 0);
    assert(!unlink(p));
    printf("PASS: symlink destination rejected, not followed\n");

    /* Reject PKG whose parent folder has been replaced by a symlink. */
    snprintf(p, sizeof(p), "%s/Update Version.pkg", updates);
    assert(checked_source(root, p, (dev_t)0x7fffffff) != 0);

    /* Simulate the separate, explicit prepare phase on TEMPORARY files only. */
    assert(prepare_title(t) == 0);
    snprintf(p, sizeof(p), "%s/ps4/CUSA01623 - Game_ Test _ PS4/app.pkg", root);
    snprintf(q, sizeof(q), "%s/ps4/CUSA01623 - Game_ Test _ PS4/patch.pkg", root);
    assert(exists(p) && exists(q));
    snprintf(p, sizeof(p), "%s/Update Old.pkg", updates); assert(exists(p));
    snprintf(p, sizeof(p), "%s/Update Much Older.pkg", updates); assert(exists(p));
    snprintf(p, sizeof(p), "%s/Foreign Update.pkg", updates); assert(exists(p));
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); assert(exists(p));
    printf("PASS: deliberate same-volume move to ps4/CUSA - sanitized TITLE; DLC intact\n");

    /* Duplicate base PKG must make this CUSA unselectable. */
    snprintf(p, sizeof(p), "%s/Duplicate Base.pkg", other); write_dummy(p, "gd CUSA01234");
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    assert(catalog_find("CUSA01234") && catalog_find("CUSA01234")->conflict);
    assert(prepare_title(catalog_find("CUSA01234")) != 0);
    printf("PASS: duplicate base is blocked rather than overwritten\n");

    /* Ambiguous same-version updates: do not select an arbitrary file. */
    snprintf(p, sizeof(p), "%s/AuxA.pkg", other); write_dummy(p, "gp CUSA01234 01.05");
    snprintf(p, sizeof(p), "%s/AuxB.pkg", other); write_dummy(p, "gp CUSA01234 01.05");
    /* The existing base is duplicated, but update ambiguity is independent. */
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    title_t *amb = catalog_find("CUSA01234");
    assert(amb && !amb->patch_conflict && amb->verified_duplicate_updates == 1);
    assert(amb->patch_path[0] && !strcmp(amb->patch_version, "01.05"));
    printf("PASS: exact byte-for-byte duplicate small updates can share one choice\n");

    /* Equal APP_VER and content id are not enough when package bytes differ. */
    snprintf(p, sizeof(p), "%s/AuxB.pkg", other);
    write_dummy(p, "gp CUSA01234 01.05 changed");
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    amb = catalog_find("CUSA01234");
    assert(amb && amb->patch_conflict && !amb->patch_path[0]);
    assert(amb->verified_duplicate_updates == 0);
    printf("PASS: distinct PKG bytes with the same version are blocked\n");

    /* Large files must NOT be considered identical from sampled data only. */
    snprintf(p, sizeof(p), "%s/AuxA.pkg", other);
    int fd = open(p, O_RDWR); assert(fd >= 0);
    assert(!ftruncate(fd, EXACT_COMPARE_MAX_BYTES + 1)); close(fd);
    snprintf(p, sizeof(p), "%s/AuxB.pkg", other);
    write_dummy(p, "gp CUSA01234 01.05");
    fd = open(p, O_RDWR); assert(fd >= 0);
    assert(!ftruncate(fd, EXACT_COMPARE_MAX_BYTES + 1)); close(fd);
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    amb = catalog_find("CUSA01234");
    assert(amb && amb->patch_conflict && amb->unverified_duplicate_updates == 1);
    assert(!amb->patch_path[0]);
    printf("PASS: matching samples of large PKGs remain inconclusive and blocked\n");
    snprintf(p, sizeof(p), "%s/AuxA.pkg", other); assert(!unlink(p));
    snprintf(p, sizeof(p), "%s/AuxB.pkg", other); assert(!unlink(p));

    /* A gp package located in a DLC folder must be reviewed, not mounted. */
    snprintf(p, sizeof(p), "%s/Alien-Isolation-DLC3.pkg", dlcs);
    write_dummy(p, "gp CUSA01623 99.99");
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    t = catalog_find("CUSA01623");
    assert(t && t->suspicious_updates == 1 && t->patch_conflict && !t->patch_path[0]);
    printf("PASS: DLC-looking gp PKG is flagged despite its SFO category\n");
    assert(!unlink(p));

    /* Bad version cannot be sorted; do not silently choose another update. */
    snprintf(p, sizeof(p), "%s/Unparseable.pkg", updates); write_dummy(p, "gp CUSA01623 unknown");
    g_title_count = 0; scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i) resolve_updates(&g_titles[i]);
    t = catalog_find("CUSA01623"); assert(t && t->invalid_updates == 1 && t->patch_conflict);
    assert(!t->patch_path[0]);
    printf("PASS: unknown APP_VER blocks automatic update selection\n");

    /* Small file metadata writes use atomic replace, reject symlink targets. */
    snprintf(p, sizeof(p), "%s/marker.txt", root); write_dummy(p, "old");
    assert(write_file(p, "new", 3, 0600) == 0);
    char readbuf[32] = {0}; FILE *f = fopen(p, "r"); assert(f);
    assert(fread(readbuf, 1, 3, f) == 3); fclose(f); assert(!strcmp(readbuf, "new"));
    snprintf(q, sizeof(q), "%s/alias", root);
    assert(symlink(p, q) == 0);
    assert(write_file(q, "damage", 6, 0600) != 0);
    memset(readbuf, 0, sizeof(readbuf)); f = fopen(p, "r"); assert(f);
    assert(fread(readbuf, 1, 3, f) == 3); fclose(f); assert(!strcmp(readbuf, "new"));
    printf("PASS: atomic file replacement and symlink protection\n");

    cleanup_fixture(root);
    return 0;
}
