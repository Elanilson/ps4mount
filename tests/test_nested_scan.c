/* Host-only read-only scanner and isolated same-volume preparation tests.
 * Simulated PKGs only. This must NEVER operate on a real PS5 mount. */
#define main ps4mount_console_main
#include "../src/main.c"
#undef main
#include <assert.h>

static char test_root[PATH_MAX];

/* Fake pkg files have "gd CUSA01623" or "gp CUSA01623" or "ac ...". */
int pkg_open(pkg_t *pkg, const char *path) {
    memset(pkg, 0, sizeof(*pkg));
    pkg->fd = open(path, O_RDONLY);
    if (pkg->fd < 0) return -1;
    char category[4] = "", id[12] = "";
    FILE *f = fdopen(dup(pkg->fd), "r");
    int ok = f && fscanf(f, "%3s %11s", category, id) == 2;
    if (f) fclose(f);
    if (!ok) { close(pkg->fd); pkg->fd = -1; return -1; }
    memcpy(pkg->header, category, 3);
    snprintf(pkg->title_id, sizeof(pkg->title_id), "%s", id);
    pkg->pkg_size = 123;
    return 0;
}
void pkg_close(pkg_t *pkg) { if (pkg->fd >= 0) close(pkg->fd); pkg->fd = -1; }
int pkg_fake_key(pkg_t *pkg) { (void)pkg; return 1; }
uint8_t *pkg_entry(pkg_t *pkg, uint32_t id, size_t *len) {
    if (id != 0x1000) return NULL;
    *len = 3;
    uint8_t *data = malloc(4);
    memcpy(data, pkg->header, 4);
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
    strcpy(sfo->items[2].sval, "01.03");
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
    snprintf(p, sizeof(p), "%s/Update Version.pkg", updates); write_dummy(p, "gp CUSA01623");
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); write_dummy(p, "ac CUSA01623");
    snprintf(p, sizeof(p), "%s/Another Game.pkg", other); write_dummy(p, "gd CUSA01234");
    snprintf(p, sizeof(p), "%s/Orphan Update.pkg", updates); write_dummy(p, "gp CUSA09999");

    g_title_count = 0; scan_nested(root, root, 0);
    title_t *t = catalog_find("CUSA01623"), *t2 = catalog_find("CUSA01234");
    assert(t && t2 && t->app_path[0] && t->patch_path[0]);
    assert(strstr(t->app_path, "Original Game.pkg"));
    assert(strstr(t->patch_path, "Update Version.pkg"));
    assert(!t->conflict && !t->patch_conflict);
    assert(!t2->patch_path[0]);
    snprintf(p, sizeof(p), "%s/app.pkg", game); assert(!exists(p));
    snprintf(p, sizeof(p), "%s/Original Game.pkg", game); assert(exists(p));
    snprintf(p, sizeof(p), "%s/Update Version.pkg", updates); assert(exists(p));
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); assert(exists(p));
    printf("PASS: recursive scanning is read-only and joins update by CUSA across folders\n");

    /* Test registration is hard-disabled without touching the file tree. */
    char msg[256];
    assert(app_add("CUSA01623", msg, sizeof(msg)) != 0);
    snprintf(p, sizeof(p), "%s/Original Game.pkg", game); assert(exists(p));
    printf("PASS: all console registration requests are rejected in preview mode\n");

    /* Simulate the separate, explicit prepare phase on TEMPORARY files only. */
    assert(prepare_title(t) == 0);
    snprintf(p, sizeof(p), "%s/ps4/CUSA01623 - Game_ Test _ PS4/app.pkg", root);
    snprintf(q, sizeof(q), "%s/ps4/CUSA01623 - Game_ Test _ PS4/patch.pkg", root);
    assert(exists(p) && exists(q));
    snprintf(p, sizeof(p), "%s/DLC.pkg", dlcs); assert(exists(p));
    printf("PASS: deliberate same-volume move to ps4/CUSA - sanitized TITLE; DLC intact\n");

    /* Duplicate base PKG must make this CUSA unselectable. */
    snprintf(p, sizeof(p), "%s/Duplicate Base.pkg", other); write_dummy(p, "gd CUSA01234");
    g_title_count = 0; scan_nested(root, root, 0);
    assert(catalog_find("CUSA01234") && catalog_find("CUSA01234")->conflict);
    assert(prepare_title(catalog_find("CUSA01234")) != 0);
    printf("PASS: duplicate base is blocked rather than overwritten\n");

    rm_tree(root);
    return 0;
}
