/* Host-only filesystem test for the unmodified production scan functions.
 * Stub PS5 services and package parsing; NEVER run this against real PKGs. */
#define main ps4mount_console_main
#include "../src/main.c"
#undef main
#include <assert.h>

/* Fake pkg files contain "gd CUSA00001", "gp CUSA00001" or "ac CUSA00001". */
int pkg_open(pkg_t *pkg, const char *path) {
    memset(pkg, 0, sizeof(*pkg));
    pkg->fd = open(path, O_RDONLY);
    if (pkg->fd < 0)
        return -1;
    char category[4] = "", id[12] = "";
    FILE *f = fdopen(dup(pkg->fd), "r");
    int ok = f && fscanf(f, "%3s %11s", category, id) == 2;
    if (f) fclose(f);
    if (!ok) {close(pkg->fd); pkg->fd=-1;return -1;}
    memcpy(pkg->header, category, sizeof(category));
    snprintf(pkg->title_id, sizeof(pkg->title_id), "%s", id);
    return 0;
}
void pkg_close(pkg_t *pkg) {if (pkg->fd >= 0) close(pkg->fd); pkg->fd = -1;}
int pkg_fake_key(pkg_t *pkg) {(void)pkg;return 1;}
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
    sfo->count = 1;
    strcpy(sfo->items[0].key, "CATEGORY");
    snprintf(sfo->items[0].sval, sizeof(sfo->items[0].sval), "%.2s", data);
    return 0;
}
const sfo_item_t *sfo_get(const sfo_t *sfo, const char *key) {
    return !strcmp(key,"CATEGORY") ? &sfo->items[0] : NULL;
}
int statfs(const char *p, struct statfs *sf) {(void)p;(void)sf;return -1;}

static void makefile(const char *p, const char *text) {
    FILE *f = fopen(p, "w"); assert(f);
    assert(fputs(text, f) >= 0); fclose(f);
}

int main(void) {
    char tmp[] = "/tmp/ps4mount-test-XXXXXX";
    char *root = mkdtemp(tmp); assert(root);
    char ps4[PATH_MAX], game[PATH_MAX], dlc[PATH_MAX], game2[PATH_MAX], path[PATH_MAX];
    snprintf(ps4,sizeof(ps4),"%s/PS4",root);assert(mkdir(ps4,0700)==0);
    snprintf(game,sizeof(game),"%s/God of War III",ps4); assert(mkdir(game,0700)==0);
    snprintf(dlc,sizeof(dlc),"%s/DLC",game);assert(mkdir(dlc,0700)==0);
    snprintf(path,sizeof(path),"%s/God of War III.pkg",game);makefile(path,"gd CUSA01623");
    snprintf(path,sizeof(path),"%s/update-v1.02.pkg",game);makefile(path,"gp CUSA01623");
    snprintf(path,sizeof(path),"%s/dlc1.pkg",dlc);makefile(path,"ac CUSA01623");
    snprintf(game2,sizeof(game2),"%s/Second Game",ps4);assert(mkdir(game2,0700)==0);
    snprintf(path,sizeof(path),"%s/game.pkg",game2);makefile(path,"gd CUSA01234");
    snprintf(path,sizeof(path),"%s/incorrect-update.pkg",game2);makefile(path,"gp CUSA09999");
    g_title_count=0;
    scan_nested(root, root, 0);
    assert(g_title_count==2);
    assert(catalog_find("CUSA01623") && !strcmp(catalog_find("CUSA01623")->dir,game));
    assert(catalog_find("CUSA01234") && !strcmp(catalog_find("CUSA01234")->dir,game2));
    snprintf(path,sizeof(path),"%s/app.pkg",game);assert(exists(path));
    snprintf(path,sizeof(path),"%s/patch.pkg",game);assert(exists(path));
    snprintf(path,sizeof(path),"%s/dlc1.pkg",dlc);assert(exists(path));
    snprintf(path,sizeof(path),"%s/patch.pkg",game2);assert(!exists(path));
    snprintf(path,sizeof(path),"%s/incorrect-update.pkg",game2);assert(exists(path));
    snprintf(path,sizeof(path),"%s/app.pkg",game2);assert(exists(path));
    /* Idempotent scan, no duplicates or further renames. */
    g_title_count=0;scan_nested(root,root,0);assert(g_title_count==2);
    printf("PASS nested folder discovery, game+patch renames, DLC preservation and rescan\n");
    rm_tree(root);
    return 0;
}
