/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* ps4mount: run PS4 fpkgs from external storage without installing them.
 *
 * Every scan:
 *   1. A PS4 app pkg found at the root of /mnt/usbN or /mnt/extN is moved to
 *      <root>/ps4/<TITLE_ID>/app.pkg (same disk, rename only).
 *   2. For each <root>/ps4/<TITLE_ID>/app.pkg the files a real install leaves
 *      behind are generated from the pkg into an internal folder next to a symlink
 *      to the pkg, that folder is mounted over /user/app/<TITLE_ID> (nullfs) and
 *      the title is added to app.db/appinfo.db.
 *   3. Mounts whose storage disappeared are released.
 * Titles installed the normal way are never touched. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "build.h"
#include "mount_policy.h"
#include "crypto.h"
#include "db.h"
#include "origin.h"
#include "pkg.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount v7-source (manual USB)"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
#define MAX_WALK_DEPTH 8
#define MAX_WALK_DIRS 20000
#define STATE_DIR "/data/ps4mount"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/ps4mount"
#define MAX_TITLES 256
#define MAX_DLC 64
#define BATCH_QUIET 10 /* seconds without writes to the system databases between groups */
#define MAX_DLC_TIDS 8

#define MAX_SCAN_PATHS 8
#define CONFIG_PATH STATE_DIR "/config.txt"
#define RESTORED_LIST STATE_DIR "/restored.txt" /* pkgs moved back: not picked up again */

/* Settings, kept in CONFIG_PATH and edited in the panel. */
typedef struct {
    int batch;      /* new games are added in groups */
    int batch_size; /* games per group */
    int dlc;        /* add-ons are moved and installed */
    char paths[MAX_SCAN_PATHS][128]; /* folders searched for pkgs, relative to each drive; "" is the root */
    int npaths;
} config_t;

static config_t g_cfg = {1, 5, 1, {""}, 1};

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    printf("[notify] %s\n", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* --- small fs helpers --- */

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    mkdir(tmp, mode);
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

/* Mounts on path from the mount table. Unlike statfs(path) this also sees a layer
 * whose source drive was disconnected (statfs on it fails). */
static int layers_on(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path))
            count++;
    free(list);
    return count;
}

/* A layer is on path but cannot be read: its drive was disconnected (and maybe
 * connected again, which gives it a new mount). */
static int dead_layer(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) && layers_on(path) > 0;
}

/* Only accept a real storage mount, not an empty /mnt/usbN directory.
 * A live mount-table entry and an accessible root are necessary; no PKG is read here.
 * Do not use a cached drive list as proof that a drive is still connected. */
static int is_mountpoint(const char *path) {
    struct statfs sf;
    struct stat st;
    if (statfs(path, &sf) || strcmp(sf.f_mntonname, path) ||
        !strcmp(sf.f_fstypename, "nullfs") || stat(path, &st) || !S_ISDIR(st.st_mode))
        return 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    struct statfs *list = calloc((size_t)n + 16, sizeof(*list));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * ((size_t)n + 16)), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path) && !strcmp(list[i].f_mntfromname, sf.f_mntfromname) &&
            !strcmp(list[i].f_fstypename, sf.f_fstypename)) { found = 1; break; }
    free(list);
    return found;
}

static int title_id_valid(const char *tid) {
    if (!tid || strlen(tid) != 9 || strncmp(tid, "CUSA", 4)) return 0;
    for (int i = 4; i < 9; i++) if (tid[i] < '0' || tid[i] > '9') return 0;
    return 1;
}

static void title_state_path(char *out, size_t n, const char *tid, const char *suffix) {
    snprintf(out, n, STATE_DIR "/%s.%s", tid, suffix);
}
static int title_is_active(const char *tid) {
    char p[96];
    title_state_path(p, sizeof(p), tid, "disabled");
    if (exists(p)) return 0; /* v6 compatibility */
    title_state_path(p, sizeof(p), tid, "active");
    return exists(p);
}

/* Atomic marker publication: an interrupted write never leaves a partial .active. */
static int set_title_active(const char *tid, int active) {
    char active_path[96], disabled_path[96], tmp[128];
    title_state_path(active_path, sizeof(active_path), tid, "active");
    title_state_path(disabled_path, sizeof(disabled_path), tid, "disabled");
    const char *dst = active ? active_path : disabled_path;
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    if (write_file(tmp, tid, strlen(tid), 0666) || rename(tmp, dst)) {
        printf("[state] could not persist %s: %s\n", dst, strerror(errno));
        unlink(tmp);
        return -1;
    }
    unlink(active ? disabled_path : active_path);
    return 0;
}

/* --- appmeta entries copied verbatim from the pkg --- */

typedef struct {
    uint32_t id;
    const char *name;
} meta_entry_t;

static const meta_entry_t k_user_meta[] = {
    {0x1200, "icon0.png"}, {0x1220, "pic0.png"},  {0x1006, "pic1.png"},          {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"}, {0x12a0, "pic0.dds"},  {0x1004, "pronunciation.xml"}, {0x100b, "shareparam.json"},
    {0, NULL},
};

static int write_entry(pkg_t *pkg, uint32_t id, const char *path) {
    size_t len;
    uint8_t *data = pkg_entry(pkg, id, &len);
    if (!data)
        return 1; /* absent entry is fine */
    int rc = write_file(path, data, len, 0777);
    free(data);
    return rc;
}

static int write_buf(const char *path, const buf_t *b) { return write_file(path, b->data, b->len, 0777); }

/* --- registration --- */

/* An add-on: <root>/ps4/<TID>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
typedef struct {
    char path[PATH_MAX];
    pkg_t pkg;
    sfo_t sfo;
    int data; /* 1: has data (mounted by the game), 0: only unlocks content */
} dlc_src_t;

/* One library folder: <root>/ps4/<TID>/app.pkg plus an optional patch.pkg and add-ons. */
typedef struct {
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    pkg_t app, patch;
    int has_patch;
    sfo_t app_sfo, patch_sfo;
    char name[128], ver[8], marker[PATH_MAX + 160];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
    dlc_src_t *dlc;
    int ndlc;
    char dlc_tids[MAX_DLC_TIDS][10]; /* add-on title IDs: /user/addcont/<TID> folders */
    int ndlc_tids;
} title_src_t;

static int read_sfo(pkg_t *pkg, sfo_t *sfo) {
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    int rc = d ? sfo_parse(d, len, sfo) : -1;
    free(d);
    return rc;
}

static void src_close(title_src_t *t) {
    pkg_close(&t->app);
    if (t->has_patch)
        pkg_close(&t->patch);
    for (int i = 0; i < t->ndlc; i++)
        pkg_close(&t->dlc[i].pkg);
    free(t->dlc);
    t->dlc = NULL;
    t->ndlc = 0;
}

static int dlc_cmp(const void *a, const void *b) {
    return strcmp(((const dlc_src_t *)a)->pkg.content_id, ((const dlc_src_t *)b)->pkg.content_id);
}

/* Add-ons are used when their folder names match the pkg (title ID and label). */
static void dlc_open(title_src_t *t) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/addcont", t->dir);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", base, e->d_name);
        DIR *l = opendir(sub);
        if (!l)
            continue;
        struct dirent *f;
        while ((f = readdir(l))) {
            if (f->d_name[0] == '.' || strlen(f->d_name) != 16)
                continue;
            if (!t->dlc && !(t->dlc = calloc(MAX_DLC, sizeof(*t->dlc))))
                break;
            if (t->ndlc == MAX_DLC)
                break;
            dlc_src_t *a = &t->dlc[t->ndlc];
            snprintf(a->path, sizeof(a->path), "%s/%s/ac.pkg", sub, f->d_name);
            if (!exists(a->path))
                continue;
            if (pkg_open(&a->pkg, a->path) || read_sfo(&a->pkg, &a->sfo)) {
                if (a->pkg.fd > 0)
                    pkg_close(&a->pkg);
                continue;
            }
            const sfo_item_t *cat = sfo_get(&a->sfo, "CATEGORY");
            /* the fake key was checked when the pkg was moved here; checking it again on
             * every scan would cost an RSA operation per add-on */
            if (!cat || strcmp(cat->sval, "ac") || strcmp(a->pkg.title_id, e->d_name) ||
                strcmp(a->pkg.content_id + 20, f->d_name)) {
                pkg_close(&a->pkg);
                continue;
            }
            char rm[96];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", a->pkg.content_id);
            if (exists(rm)) { /* deleted on the console */
                pkg_close(&a->pkg);
                continue;
            }
            a->data = a->pkg.header[0x77] == 0x1b; /* content type: 0x1b with data, 0x1c license only */
            t->ndlc++;
            int known = 0;
            for (int i = 0; i < t->ndlc_tids; i++)
                known |= !strcmp(t->dlc_tids[i], e->d_name);
            if (!known && t->ndlc_tids < MAX_DLC_TIDS)
                snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", e->d_name);
        }
        closedir(l);
    }
    closedir(d);
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

static int first_time(const char *path);

static int src_open(title_src_t *t, const char *root, const char *tid) {
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "%s/ps4/%s", root, tid);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    if (pkg_open(&t->app, t->app_path) || read_sfo(&t->app, &t->app_sfo)) {
        printf("[scan] %s: unreadable app pkg\n", t->app_path);
        if (t->app.fd >= 0) pkg_close(&t->app);
        return -1;
    }
    if (exists(t->patch_path)) {
        const sfo_item_t *gv = sfo_get(&t->app_sfo, "VERSION");
        if (pkg_open(&t->patch, t->patch_path) || read_sfo(&t->patch, &t->patch_sfo)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "Update ignored: patch.pkg could not be read");
            if (t->patch.fd > 0)
                pkg_close(&t->patch);
        } else {
            /* An update only fits the title it was made for. (Master VERSION may differ:
             * installed games run with base 01.02 + update 01.00.) */
            const sfo_item_t *uv = sfo_get(&t->patch_sfo, "VERSION");
            const char *gver = gv ? gv->sval : "?", *uver = uv ? uv->sval : "?";
            if (strcmp(t->patch.title_id, tid)) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         "Update ignored: it is for %s version %s, the game is %s version %s", t->patch.title_id, uver,
                         tid, gver);
                pkg_close(&t->patch);
            } else {
                t->has_patch = 1;
            }
        }
        if (t->update_issue[0] && first_time(t->patch_path)) {
            const sfo_item_t *title = sfo_get(&t->app_sfo, "TITLE");
            notify("%s: %s", title ? title->sval : tid, t->update_issue);
        }
    }
    const sfo_t *eff = t->has_patch ? &t->patch_sfo : &t->app_sfo;
    const sfo_item_t *it = sfo_get(eff, "TITLE");
    snprintf(t->name, sizeof(t->name), "%s", it ? it->sval : tid);
    it = sfo_get(eff, "APP_VER");
    snprintf(t->ver, sizeof(t->ver), "%s", it && !it->is_int ? it->sval : "01.00");
    char digest[65] = "";
    if (t->has_patch)
        for (int i = 0; i < 32; i++)
            snprintf(digest + i * 2, 3, "%02x", t->patch.digest[i]);
    snprintf(t->marker, sizeof(t->marker), "%s\n%s\n", t->dir, t->has_patch ? digest : "nopatch");
    if (g_cfg.dlc)
        dlc_open(t);
    if (t->ndlc) {
        /* one line for all add-ons, so markers of titles without add-ons stay as before */
        uint8_t h[32];
        buf_t all = {0};
        for (int i = 0; i < t->ndlc; i++)
            buf_put(&all, t->dlc[i].pkg.digest, 32);
        sha256(all.data, all.len, h);
        buf_free(&all);
        size_t n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "dlc %d ", t->ndlc);
        for (int i = 0; i < 32; i++) {
            n = strlen(t->marker);
            snprintf(t->marker + n, sizeof(t->marker) - n, "%02x", h[i]);
        }
        n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "\n");
    }
    return 0;
}

static void write_meta(pkg_t *pkg, const char *meta_dir, int *rc) {
    char q[PATH_MAX];
    for (const meta_entry_t *m = k_user_meta; m->name; m++) {
        snprintf(q, sizeof(q), "%s/%s", meta_dir, m->name);
        *rc |= write_entry(pkg, m->id, q) < 0;
    }
    for (uint32_t id = 0x1260; id <= 0x127f; id++) {
        size_t len;
        uint8_t *d = pkg_entry(pkg, id, &len);
        if (!d)
            continue;
        snprintf(q, sizeof(q), "%s/changeinfo", meta_dir);
        mkdirs(q, 0777);
        if (id == 0x1260)
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo.xml", meta_dir);
        else
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo_%02u.xml", meta_dir, id - 0x1261);
        *rc |= write_file(q, d, len, 0777);
        free(d);
    }
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void hold_path(char *out, size_t n, const char *kind, const char *tid) {
    snprintf(out, n, HOLD_DIR "/%s/%s", kind, tid);
}

/* Fresh internal folder for kind ("app"/"patch") with name -> pkg_path. */
static int hold_prepare(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char dir[PATH_MAX], link[PATH_MAX];
    hold_path(dir, sizeof(dir), kind, tid);
    mkdirs(dir, 0777);
    snprintf(link, sizeof(link), "%s/%s", dir, name);
    unlink(link);
    if (symlink(pkg_path, link)) {
        printf("[reg] symlink %s: %s\n", link, strerror(errno));
        return -1;
    }
    return 0;
}

/* The link of kind exists and reaches its pkg. */
static int hold_ready(const char *kind, const char *tid, const char *name) {
    char p[PATH_MAX];
    hold_path(p, sizeof(p), kind, tid);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/%s", name);
    return exists(p);
}

static uint8_t *read_all(const char *path, size_t *len) {
    struct stat st;
    *len = 0;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > (1 << 20))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    uint8_t *d = malloc((size_t)st.st_size);
    if (d && read(fd, d, (size_t)st.st_size) != st.st_size) {
        free(d);
        d = NULL;
    }
    close(fd);
    if (d)
        *len = (size_t)st.st_size;
    return d;
}

/* Adds or replaces the license slot of pkg in the rif/idx of its content ID prefix,
 * keeping the slots of the game and of other add-ons. */
static int place_license(pkg_t *pkg) {
    size_t lic_len, rl, il;
    uint8_t *lic = pkg_entry(pkg, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        free(lic);
        printf("[reg] %s: no license.dat\n", pkg->content_id);
        return -1;
    }
    char rp[96], ip[96];
    snprintf(rp, sizeof(rp), "/user/license/fake%.19s.rif", pkg->content_id);
    snprintf(ip, sizeof(ip), "/user/license/fake%.19s.idx", pkg->content_id);
    uint8_t *old_rif = read_all(rp, &rl), *old_idx = read_all(ip, &il);
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, old_rif, rl, old_idx, il, pkg->content_id, lic);
    int rc = write_buf(rp, &rif) | write_buf(ip, &idx);
    free(lic);
    free(old_rif);
    free(old_idx);
    buf_free(&rif);
    buf_free(&idx);
    return rc ? -1 : 0;
}

/* Internal folder of an add-on: HOLD_DIR/addcont/<ADD-ON TID>/<LABEL>, mounted (by
 * title ID) over /user/addcont/<ADD-ON TID>. It holds ac.pkg as a symlink to the pkg on
 * the drive plus ac.json/ac.pbm. Also writes the PlayGo records, the license slot and,
 * for add-ons with data, the icon. */
static int place_dlc(dlc_src_t *a) {
    pkg_t *pkg = &a->pkg;
    const sfo_item_t *v = sfo_get(&a->sfo, "VERSION");
    const char *ver = v && !v->is_int ? v->sval : "01.00", *label = pkg->content_id + 20;
    char dir[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(dir, sizeof(dir), HOLD_DIR "/addcont/%s/%s", pkg->title_id, label);
    mkdirs(dir, 0777);
    snprintf(q, sizeof(q), "%s/ac.pkg", dir);
    unlink(q);
    int rc = 0;
    if (symlink(a->path, q)) {
        printf("[reg] symlink %s: %s\n", q, strerror(errno));
        rc = -1;
    }
    buf_t json = {0}, pbm = {0}, pdb = {0};
    build_app_json(&json, pkg, a->path);
    build_app_pbm(&pbm, pkg, ver, a->data ? PBM_ADDCONT : PBM_LICENSE);
    build_ac_pdb(&pdb, pkg, a->path, &json);
    snprintf(q, sizeof(q), "%s/ac.json", dir);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/ac.pbm", dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/ac.pbm.backup", dir);
    rc |= write_buf(q, &pbm);
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s/00", pkg->content_id);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/f0.pdb", p);
    rc |= write_file(q, "", 0, 0777);
    rc |= place_license(pkg);
    if (a->data) {
        snprintf(p, sizeof(p), "/user/appmeta/addcont/%s/%s", pkg->title_id, label);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/icon0.png", p);
        rc |= write_entry(pkg, 0x1200, q) < 0;
    }
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&pdb);
    if (rc)
        printf("[reg] add-on %s: could not write its files\n", pkg->content_id);
    return rc ? -1 : 0;
}

/* The ac.pkg link of every add-on exists and reaches its pkg. */
static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id, *cid = app->content_id;
    const sfo_item_t *av = sfo_get(&t->app_sfo, "APP_VER");
    const char *app_ver = av && !av->is_int ? av->sval : "01.00";
    char p[PATH_MAX], q[PATH_MAX], hold[PATH_MAX];
    int rc = 0;

    snprintf(p, sizeof(p), "/user/appmeta/%s", tid);
    mkdirs(p, 0777);
    write_meta(app, p, &rc);
    if (pat)
        write_meta(pat, p, &rc); /* the update's artwork wins, as with a real install */

    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/param.sfo", p);
    rc |= write_entry(pat ? pat : app, 0x1000, q) != 0;
    snprintf(q, sizeof(q), "%s/nptitle.dat", p);
    rc |= write_entry(app, 0x402, q) < 0;
    snprintf(q, sizeof(q), "%s/npbind.dat", p);
    rc |= write_entry(app, 0x403, q) < 0;

    if (place_license(app))
        return -1;
    for (int i = 0; i < t->ndlc; i++)
        rc |= place_dlc(&t->dlc[i]);

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0}, pdb = {0}, patch_pdb = {0};
    build_app_json(&json, app, t->app_path);
    build_app_pbm(&pbm, app, app_ver, PBM_APP);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, t->patch_path);
        build_app_pbm(&ppbm_patch, pat, t->ver, PBM_PATCH);
        pr = (patch_ref_t){pat, ppg, ppl, t->ver, t->patch_path, &pjson};
        prp = &pr;
    }
    if (build_app_xml(&xml, pg, pl) || build_pfs_pbm(&ppbm, app, pg, pl, prp) ||
        build_pdb(&pdb, app, t->ver, t->app_path, &json, pg, pl, prp) ||
        (pat && (build_app_xml(&pxml, ppg, ppl) || build_patch_pdb(&patch_pdb, &pr)))) {
        printf("[reg] %s: playgo-chunk.dat unusable\n", tid);
        rc = -1;
        goto out;
    }
    rc |= hold_prepare("app", tid, "app.pkg", t->app_path);
    hold_path(hold, sizeof(hold), "app", tid);
    snprintf(q, sizeof(q), "%s/app.json", hold);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", hold);
    rc |= write_buf(q, &xml);
    snprintf(p, sizeof(p), "/system_data/playgo/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/pfs.pbm", p);
    rc |= write_buf(q, &ppbm);
    snprintf(q, sizeof(q), "%s/pfs.pbm.backup", p);
    rc |= write_buf(q, &ppbm);
    snprintf(p, sizeof(p), "/user/playgo/app/%s/00", cid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(p, sizeof(p), "/user/playgo/patch/%s", cid);
    if (pat) {
        rc |= hold_prepare("patch", tid, "patch.pkg", t->patch_path);
        hold_path(hold, sizeof(hold), "patch", tid);
        snprintf(q, sizeof(q), "%s/patch.json", hold);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", hold);
        rc |= write_buf(q, &pxml);
        snprintf(p, sizeof(p), "/user/playgo/patch/%s/00/00.00", cid);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/d0.pdb", p);
        rc |= write_buf(q, &patch_pdb);
        snprintf(q, sizeof(q), "%s/d1.pdb", p);
        rc |= write_buf(q, &patch_pdb);
    } else {
        rm_tree(p);
        hold_path(hold, sizeof(hold), "patch", tid);
        rm_tree(hold);
    }
out:
    free(pg);
    free(ppg);
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&xml);
    buf_free(&pjson);
    buf_free(&ppbm_patch);
    buf_free(&pxml);
    buf_free(&ppbm);
    buf_free(&pdb);
    buf_free(&patch_pdb);
    return rc ? -1 : 0;
}

#define IOV(s) {(void *)(s), strlen(s) + 1}

/* Titles this daemon serves. */
typedef struct {
    int used, has_patch, running, ndlc, ndlc_tids;
    char dlc_tids[MAX_DLC_TIDS][10];
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
static int g_manual_scan; /* "Scan for games" in the panel */
static int g_deleting; /* a delete screen is open: game folders are unmounted */
static time_t g_deleting_since;
static time_t g_deleting_close_at; /* set when the uninstall finished */

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our folder, -1 something else (like a USB
 * folder mounted by an older PS4Mount). */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    return strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir) ? -1 : 1;
}

static int nullfs_mount(const char *dir, const char *target) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    if (nmount(iov, 6, 0)) {
        printf("[mount] %s -> %s: %s\n", dir, target, strerror(errno));
        return -1;
    }
    return 0;
}

/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
/* Sandbox mounts of running titles, read from the mount table at most once a second
 * (the table holds over a thousand entries; reading it per title would be wasteful). */
static char (*g_sandboxes)[10];
static int g_nsandboxes;
static time_t g_sandboxes_at;

static void refresh_sandboxes(void) {
    if (g_sandboxes && time(NULL) == g_sandboxes_at)
        return;
    g_sandboxes_at = time(NULL);
    g_nsandboxes = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    char (*ids)[10] = realloc(g_sandboxes, sizeof(*ids) * (size_t)(n + 16));
    if (ids)
        g_sandboxes = ids;
    if (!list || !ids) {
        free(list);
        return;
    }
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    static const char prefix[] = "/mnt/sandbox/pfsmnt/";
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname;
        if (strlen(on) != sizeof(prefix) - 1 + 9 + 5 || strncmp(on, prefix, sizeof(prefix) - 1) ||
            strcmp(on + sizeof(prefix) - 1 + 9, "-app0"))
            continue;
        snprintf(g_sandboxes[g_nsandboxes++], 10, "%.9s", on + sizeof(prefix) - 1);
    }
    free(list);
}

static int title_running(const char *tid) {
    refresh_sandboxes();
    for (int i = 0; i < g_nsandboxes; i++)
        if (!strcmp(g_sandboxes[i], tid))
            return 1;
    return 0;
}

/* The source of each layer comes from the mount table, which still reports
 * dead USB layers when statfs(target) itself fails. No foreign layer is removed. */
static int owned_layers(const char *target, const char *expected, int *count) {
    *count = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n < 0) return -1;
    if (!n) return 0;
    struct statfs *list = calloc((size_t)n + 16, sizeof(*list));
    if (!list) return -1;
    n = getfsstat(list, (long)(sizeof(*list) * ((size_t)n + 16)), MNT_NOWAIT);
    if (n < 0) { free(list); return -1; }
    int foreign = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].f_mntonname, target)) continue;
        (*count)++;
        if (!mount_layer_is_owned(list[i].f_fstypename, list[i].f_mntfromname, expected))
            foreign = 1;
    }
    free(list);
    return foreign ? -1 : 0;
}

/* Explicitly limited to the PS4Mount hold folder. When the table is unreadable,
 * fail closed: it is worse to remove another program's mount than to reject a click. */
static int unmount_owned(const char *target, const char *expected) {
    int count = 0;
    if (owned_layers(target, expected, &count)) {
        printf("[mount] %s: foreign/unreadable layer, refusing unmount\n", target);
        return -1;
    }
    for (int i = 0; i < count && i < 8; i++) {
        /* Non-forced first; forced removal only for a verified owned, dead layer. */
        int force = dead_layer(target) ? MNT_FORCE : 0;
        if (unmount(target, force)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        int remaining = 0;
        if (owned_layers(target, expected, &remaining)) return -1;
        if (remaining == 0) return 0;
    }
    return count < 8 ? 0 : -1;
}

static int mount_dir(const char *dir, const char *target) {
    if (!exists(dir)) {
        printf("[mount] %s: prepared source does not exist\n", dir);
        return -1;
    }
    char parent[64];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) { *slash = 0; mkdir(parent, 0770); }
    mkdir(target, 0777);
    int count = 0;
    if (owned_layers(target, dir, &count)) {
        printf("[mount] %s: a foreign mount is present, refusing replacement\n", target);
        return -1;
    }
    if (count) {
        if (!dead_layer(target) && top_layer(target, dir) == 1)
            return 0;
        if (unmount_owned(target, dir)) return -1;
    }
    if (nullfs_mount(dir, target)) return -1;
    return top_layer(target, dir) == 1 ? 0 : -1;
}

/* Check all title layers before changing any of them: partial foreign conflicts
 * must not trigger removal of the remaining valid game layers. */
static int title_mounts_owned(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    for (int i = -2; i < ndtids; i++) {
        const char *kind = i == -2 ? "app" : i == -1 ? "patch" : "addcont";
        const char *id = i < 0 ? tid : dtids[i];
        snprintf(target, sizeof(target), "/user/%s/%s", kind, id);
        hold_path(hold, sizeof(hold), kind, id);
        int count = 0;
        if (owned_layers(target, hold, &count)) return -1;
    }
    return 0;
}

static int unmount_title(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    if (title_mounts_owned(tid, dtids, ndtids)) return -1;
    for (int i = -2; i < ndtids; i++) {
        const char *kind = i == -2 ? "app" : i == -1 ? "patch" : "addcont";
        const char *id = i < 0 ? tid : dtids[i];
        snprintf(target, sizeof(target), "/user/%s/%s", kind, id);
        hold_path(hold, sizeof(hold), kind, id);
        if (unmount_owned(target, hold)) return -1;
    }
    return 0;
}

/* Add-ons installed the normal way: their folder is never covered by a mount. */
static int addcont_installed(const char *dtid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/addcont/%s", dtid);
    if (mounted_from(target, NULL, 0))
        return 0;
    DIR *d = opendir(target);
    if (!d)
        return 0;
    struct dirent *e;
    int used = 0;
    while (!used && (e = readdir(d)))
        used = e->d_name[0] != '.';
    closedir(d);
    return used;
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update and
 * /user/addcont/<ADD-ON TID> for each add-on title. */
static int mount_paths(const char *tid, int has_patch, char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    if (title_mounts_owned(tid, dtids, ndtids)) {
        printf("[mount] %s: foreign layer, no changes made\n", tid);
        return -1;
    }
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (mount_dir(hold, target))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(hold, sizeof(hold), "patch", tid);
    if (has_patch ? mount_dir(hold, target) : unmount_owned(target, hold))
        return -1;
    for (int i = 0; i < ndtids; i++) {
        if (addcont_installed(dtids[i])) {
            printf("[mount] /user/addcont/%s holds add-ons installed the normal way, not mounted\n", dtids[i]);
            continue;
        }
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        hold_path(hold, sizeof(hold), "addcont", dtids[i]);
        if (mount_dir(hold, target))
            return -1;
    }
    return 0;
}

static int mount_title(title_src_t *t) {
    return mount_paths(t->app.title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

/* A successful call alone is not evidence of a complete game mount. Verify
 * the base, update (when present) and each owned add-on layer against statfs. */
static int all_layers_mounted(const title_src_t *t) {
    const char *tid = t->app.title_id;
    char path[64], hold[PATH_MAX];
    snprintf(path, sizeof(path), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (top_layer(path, hold) != 1) return 0;
    if (t->has_patch) {
        snprintf(path, sizeof(path), "/user/patch/%s", tid);
        hold_path(hold, sizeof(hold), "patch", tid);
        if (top_layer(path, hold) != 1) return 0;
    }
    for (int i = 0; i < t->ndlc_tids; i++) {
        const char *id = t->dlc_tids[i];
        if (addcont_installed(id)) continue; /* normal installed DLC is not ours */
        snprintf(path, sizeof(path), "/user/addcont/%s", id);
        hold_path(hold, sizeof(hold), "addcont", id);
        if (top_layer(path, hold) != 1) return 0;
    }
    return 1;
}

static void manage(const title_src_t *t) {
    managed_t *m = managed_by(t->app.title_id, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m)
        return;
    /* keep the runtime state across rescans */
    managed_t keep = *m;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    if (keep.used) {
        m->running = keep.running;
        m->running_since = keep.running_since;
    }
    m->has_patch = t->has_patch;
    m->ndlc = t->ndlc;
    m->ndlc_tids = t->ndlc_tids;
    memcpy(m->dlc_tids, t->dlc_tids, sizeof(m->dlc_tids));
    snprintf(m->tid, sizeof(m->tid), "%s", t->app.title_id);
    snprintf(m->cid, sizeof(m->cid), "%s", t->app.content_id);
    snprintf(m->name, sizeof(m->name), "%s", t->name);
    snprintf(m->dir, sizeof(m->dir), "%s", t->dir);
}


/* Real installs keep app.pkg on the internal drive; never touch those. */
static int installed_internally(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (mounted_from(p, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "/user/app/%s/app.pkg", tid);
    return exists(p);
}

static int marker_matches(const char *tid, const char *expected) {
    char path[64], buf[PATH_MAX + 256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strcmp(buf, expected);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
/* The title is served from another connected drive: the marker names another
 * folder whose game pkg is still there. The first drive keeps it, so the same game
 * on two drives is not switched (and registered again) on every scan. */
static int served_elsewhere(const title_src_t *t) {
    char path[64], buf[PATH_MAX + 256] = {0}, pkg[PATH_MAX + 16];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", t->app.title_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!strcmp(buf, t->dir))
        return 0;
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", buf);
    return exists(pkg);
}

static void pump_events(int ms);

/* The launch check compares /system_data/priv/appmeta/<TID>/param.sfo with the
 * update's param.sfo byte for byte; AppInstallAll writes the game's one there. */
static void enforce_patch_sfo(title_src_t *t) {
    if (!t->has_patch)
        return;
    size_t len;
    uint8_t *sfo = pkg_entry(&t->patch, 0x1000, &len);
    if (!sfo)
        return;
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", t->app.title_id);
    int fd = open(path, O_RDONLY), same = 0;
    if (fd >= 0) {
        uint8_t *cur = malloc(len + 1);
        same = cur && read(fd, cur, len + 1) == (ssize_t)len && !memcmp(cur, sfo, len);
        free(cur);
        close(fd);
    }
    if (!same) {
        printf("[reg] %s: priv param.sfo was not the update's, rewritten\n", t->app.title_id);
        write_file(path, sfo, len, 0777);
    }
    free(sfo);
}

/* How titles are registered by the system, chosen from the firmware at startup:
 *   REG_ALL      sceAppInstUtilAppInstallAll: the system registers every new title under
 *                /user/app. Used from 6.00 on (works on 6.50 and 13.20).
 *   REG_TITLEDIR sceAppInstUtilAppInstallTitleDir, one title at a time (works on 5.50). On
 *                5.50 InstallAll made the system check its whole app database and flag it
 *                as corrupted; below 6.00 the per-title call is used when the firmware has
 *                it (it was removed in newer firmwares: not present on 13.20).
 *   REG_NONE     neither is safe or available: no games are added. */
#ifndef MIN_FW_INSTALL_ALL
#define MIN_FW_INSTALL_ALL 6
#endif

enum { REG_ALL, REG_TITLEDIR, REG_NONE };

typedef int (*title_dir_fn)(const char *title_id, const char *dir, void *reserved);

static int g_reg_mode = REG_ALL;
static title_dir_fn g_title_dir;
static char g_fw_text[16] = "unknown";

static const char *reg_mode_name(void) {
    return g_reg_mode == REG_ALL ? "AppInstallAll" : g_reg_mode == REG_TITLEDIR ? "AppInstallTitleDir" : "none";
}

static void check_firmware(void) {
    uint32_t fw = kernel_get_fw_version();
    unsigned major = ((fw >> 28) & 0xf) * 10 + ((fw >> 24) & 0xf), minor = ((fw >> 20) & 0xf) * 10 + ((fw >> 16) & 0xf);
    snprintf(g_fw_text, sizeof(g_fw_text), "%u.%02u", major, minor);
    /* looked up at runtime: linking it would stop the payload from loading where it is gone */
    uint32_t h = 0;
    if (!kernel_dynlib_handle(getpid(), "libSceAppInstUtil.sprx", &h))
        g_title_dir = (title_dir_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppInstallTitleDir");
    if (major >= MIN_FW_INSTALL_ALL)
        g_reg_mode = REG_ALL;
    else
        g_reg_mode = g_title_dir ? REG_TITLEDIR : REG_NONE;
    printf("[main] firmware %s (0x%08x), AppInstallTitleDir %s, registration: %s\n", g_fw_text, fw,
           g_title_dir ? "present" : "missing", reg_mode_name());
}

/* The system registration runs in its own thread: after rest mode InstallAll was seen to
 * block for good, which froze the whole daemon (panel, klog, stop request). A call still
 * running is never doubled; titles keep being checked in the database. */
static volatile int g_install_busy, g_install_rc;
static char g_install_tids[MAX_TITLES][10];
static int g_install_n;

static int register_call(const char *tid) {
    if (g_reg_mode == REG_TITLEDIR) {
        int rc = g_title_dir(tid, "/user/app/", NULL);
        printf("[reg] %s: AppInstallTitleDir=0x%08x\n", tid, rc);
        return rc == (int)0x80990002 ? 0 : rc; /* 0x80990002: already known, restored */
    }
    return sceAppInstUtilAppInstallAll(NULL);
}

static void *install_thread(void *arg) {
    (void)arg;
    int rc = 0;
    if (g_reg_mode == REG_TITLEDIR) {
        for (int i = 0; i < g_install_n; i++) {
            int r = register_call(g_install_tids[i]);
            if (r)
                rc = r;
        }
    } else {
        rc = register_call(NULL);
    }
    g_install_rc = rc;
    g_install_busy = 0;
    return NULL;
}

/* Registers the titles of tids that are not registered yet. */
static int install_all_start(char (*tids)[10], int n) {
    if (g_reg_mode == REG_NONE)
        return -1; /* never where it corrupts the database */
    if (g_install_busy) {
        printf("[reg] registration still running from an earlier call\n");
        return -1;
    }
    g_install_n = 0;
    for (int i = 0; i < n && g_install_n < MAX_TITLES; i++)
        if (!db_is_registered(tids[i]))
            snprintf(g_install_tids[g_install_n++], 10, "%s", tids[i]);
    g_install_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, install_thread, NULL)) {
        install_thread(NULL); /* no thread: call it here */
        return 0;
    }
    pthread_detach(th);
    return 0;
}

/* Registration runs in three steps so a group of games costs the system one
 * AppInstallAll and no write to its databases while it is still busy:
 *   1. register_prepare: files and mounts of each game of the group;
 *   2. one sceAppInstUtilAppInstallAll, then wait until ShellCore has the rows;
 *   3. register_finish: checks the title is registered, writes the marker. */
/* Max mtime of the system databases (and their journals): ShellCore is busy while it changes. */
static time_t db_mtime(void) {
    static const char *files[] = {"/system_data/priv/mms/app.db", "/system_data/priv/mms/app.db-journal",
                                  "/system_data/priv/mms/appinfo.db", "/system_data/priv/mms/appinfo.db-journal",
                                  "/system_data/priv/mms/addcont.db", NULL};
    time_t t = 0;
    struct stat st;
    for (int i = 0; files[i]; i++)
        if (!stat(files[i], &st) && st.st_mtime > t)
            t = st.st_mtime;
    return t;
}

/* Waits until the system databases were not written for quiet seconds (max limit). */
static void wait_db_quiet(int quiet, int limit) {
    time_t start = time(NULL), last = db_mtime(), changed = time(NULL);
    while (time(NULL) - changed < quiet && time(NULL) - start < limit) {
        pump_events(1000);
        time_t m = db_mtime();
        if (m != last) {
            last = m;
            changed = time(NULL);
        }
    }
}

/* ShellCore registers every new title under /user/app itself; this also refreshes the
 * home screen and fetches the PSN concept. Waits while titles keep arriving. */
static void install_all(char (*tids)[10], int n) {
    int need = 0;
    for (int i = 0; i < n; i++)
        need += !db_is_registered(tids[i]);
    if (!need)
        return;
    install_all_start(tids, n);
    int done = n - need, idle = 0;
    while (done < n && idle < 30) {
        pump_events(1000);
        int now = 0;
        for (int i = 0; i < n; i++)
            now += db_is_registered(tids[i]);
        idle = now > done ? 0 : idle + 1;
        done = now;
    }
    printf("[reg] %s=%s0x%08x: %d of %d title(s) registered\n", reg_mode_name(), g_install_busy ? "(still running) " : "",
           g_install_rc, done, n);
}

/* The system databases are only ever written by the system: ShellCore keeps them
 * open with its own cache, and rows written from outside make it flag app.db as
 * corrupted (and rebuild it at the next boot). A title the system did not register
 * is tried again later. */
static int register_finish(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (!db_is_registered(tid)) {
        printf("[reg] %s: not registered by the system\n", tid);
        return -1;
    }
    enforce_patch_sfo(t);
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666))
        return -1;
    manage(t);
    return 0;
}

/* Logs a skipped root file once per daemon run (any number of files). */
static int first_time(const char *path) {
    static char **seen;
    static size_t n, cap;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(seen[i], path))
            return 0;
    if (n == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        char **grown = realloc(seen, ncap * sizeof(*seen));
        if (!grown)
            return 1;
        seen = grown;
        cap = ncap;
    }
    seen[n] = strdup(path);
    if (seen[n])
        n++;
    return 1;
}


/* 1 when sfo lists the add-on prefix (UP0000-CUSA00000_00) as one of its add-on services. */
static int sfo_serves(const sfo_t *sfo, const char *prefix) {
    char key[32];
    for (int i = 1; i <= 7; i++) {
        snprintf(key, sizeof(key), "SERVICE_ID_ADDCONT_ADD_%d", i);
        const sfo_item_t *it = sfo_get(sfo, key);
        if (it && !it->is_int && !strncmp(it->sval, prefix, 19))
            return 1;
    }
    return 0;
}

/* The game on this drive an add-on belongs to: same title ID, or a game (or its
 * update) that names the add-on's content ID prefix in SERVICE_ID_ADDCONT_ADD_n
 * (regional versions share add-ons this way). */
static int find_dlc_game(const char *root, const pkg_t *dlc, char *tid) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, dlc->title_id);
    if (exists(p)) {
        snprintf(tid, 10, "%s", dlc->title_id);
        return 0;
    }
    DIR *d = opendir(lib);
    if (!d)
        return -1;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        static const char *names[] = {"app.pkg", "patch.pkg"};
        for (int i = 0; i < 2 && !found; i++) {
            pkg_t pkg;
            sfo_t sfo;
            snprintf(p, sizeof(p), "%s/%s/%s", lib, e->d_name, names[i]);
            if (!exists(p) || pkg_open(&pkg, p))
                continue;
            found = !read_sfo(&pkg, &sfo) && sfo_serves(&sfo, dlc->content_id);
            pkg_close(&pkg);
        }
        if (found)
            snprintf(tid, 10, "%s", e->d_name);
    }
    closedir(d);
    return found ? 0 : -1;
}

/* Add-ons left in place during one adoption run, reported in one notification. */
static int g_dlc_orphans, g_dlc_dupes;

/* Add-ons waiting for their game: checked again only when the games of the drive
 * change (the check opens the pkg of every game). */
typedef struct {
    char *path;
    unsigned long sig;
} waiting_t;

static waiting_t *g_waiting;
static size_t g_nwaiting, g_capwaiting;
static unsigned long g_lib_sig; /* games of the drive being scanned */

/* Signature of the games in <root>/ps4: changes when a game is added or removed. */
static unsigned long lib_signature(const char *root) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    struct dirent *e;
    unsigned long sig = 5381;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
        if (!exists(p))
            continue;
        unsigned long h = 0;
        for (const char *c = e->d_name; *c; c++)
            h = h * 33 + (unsigned char)*c;
        sig += h; /* order independent */
    }
    closedir(d);
    return sig;
}

static waiting_t *waiting_find(const char *path) {
    for (size_t i = 0; i < g_nwaiting; i++)
        if (!strcmp(g_waiting[i].path, path))
            return &g_waiting[i];
    return NULL;
}

static void waiting_set(const char *path, unsigned long sig) {
    waiting_t *w = waiting_find(path);
    if (w) {
        w->sig = sig;
        return;
    }
    if (g_nwaiting == g_capwaiting) {
        size_t cap = g_capwaiting ? g_capwaiting * 2 : 64;
        waiting_t *grown = realloc(g_waiting, cap * sizeof(*grown));
        if (!grown)
            return;
        g_waiting = grown;
        g_capwaiting = cap;
    }
    if ((g_waiting[g_nwaiting].path = strdup(path)))
        g_waiting[g_nwaiting++].sig = sig;
}

static void waiting_drop(const char *path) {
    waiting_t *w = waiting_find(path);
    if (!w)
        return;
    free(w->path);
    *w = g_waiting[--g_nwaiting];
}

/* Add-ons of root still waiting for their game. */
static const char *drive_rel(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && path[n] == '/' ? path + n + 1 : path;
}

/* Records where a pkg moved into <root>/ps4/<TID>/<rel> came from. */
static void record_origin(const char *root, const char *tid, const char *rel, const char *from) {
    char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    if (origin_add(game, rel, drive_rel(root, from)))
        printf("[scan] %s: could not record where %s came from\n", game, rel);
}

/* Moves an add-on pkg (ac) into <root>/ps4/<GAME>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
static int adopt_dlc(const char *root, const char *name, const char *path, pkg_t *pkg) {
    char game[10], dst[PATH_MAX];
    if (find_dlc_game(root, pkg, game)) {
        if (first_time(path)) {
            /* a game served from another drive is mounted there too: only a real install counts */
            printf("[scan] %s: no game for add-on %s on the drive%s, waiting\n", name, pkg->content_id,
                   installed_internally(pkg->title_id) ? " (that title is installed on the console)" : "");
            g_dlc_orphans++;
        }
        waiting_set(path, g_lib_sig);
        return 0;
    }
    waiting_drop(path);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s", root, game, pkg->title_id, pkg->content_id + 20);
    mkdirs(dst, 0777);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s/ac.pkg", root, game, pkg->title_id, pkg->content_id + 20);
    if (exists(dst)) {
        if (first_time(path)) {
            printf("[scan] %s: %s already has this add-on, the copy was left in place\n", name, game);
            g_dlc_dupes++;
        }
        return 0;
    }
    if (rename(path, dst)) {
        printf("[scan] move %s: %s\n", path, strerror(errno));
        return 0;
    }
    printf("[scan] moved %s -> %s\n", path, dst);
    char rel[64];
    snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", pkg->title_id, pkg->content_id + 20);
    record_origin(root, game, rel, path);
    return 1;
}

/* Moves PS4 game (gd) and update (gp) pkgs found in dir (the drive root or a scan
 * folder) into <root>/ps4/<TID>/app.pkg and patch.pkg, then add-ons (ac) next to
 * their game. */
static int adopt_dir_pkgs(const char *root, const char *dir) {
    int moved = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    for (int pass = 0; pass < 2; pass++, rewinddir(d)) {
    if (pass == 1)
        g_lib_sig = lib_signature(root); /* games moved in pass 0 count */
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".pkg"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat rs;
        if (!stat(path, &rs) && restored_has(RESTORED_LIST, drive_rel(root, path), (uint64_t)rs.st_size)) {
            if (first_time(path))
                printf("[scan] skip %s (moved back from ps4/ on request)\n", path);
            continue;
        }
        waiting_t *w = waiting_find(path);
        if (w && w->sig == g_lib_sig)
            continue; /* waiting add-on, and no new game since the last check */
        pkg_t pkg;
        if (pkg_open(&pkg, path)) {
            if (pass == 0 && first_time(path))
                printf("[scan] skip %s (cannot read the pkg header: not a PS4 pkg, or unreadable)\n", path);
            continue;
        }
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        int is_app = cat && !strcmp(cat->sval, "gd"), is_patch = cat && !strcmp(cat->sval, "gp");
        int is_dlc = cat && !strcmp(cat->sval, "ac");
        if (is_dlc || pass == 1) {
            /* add-ons go in the second pass: their game may be in this folder too */
            if (pass == 1 && is_dlc && strncmp(pkg.title_id, "CUSA", 4)) {
                if (first_time(path))
                    printf("[scan] skip %s (add-on %s is not for a PS4 title)\n", path, pkg.content_id);
            } else if (pass == 1 && is_dlc) {
                if (!g_cfg.dlc) {
                    if (first_time(path))
                        printf("[scan] skip %s (add-ons are turned off)\n", path);
                } else if (pkg_fake_key(&pkg))
                    moved += adopt_dlc(root, e->d_name, path, &pkg);
                else if (first_time(path))
                    printf("[scan] skip %s (not a fake add-on pkg)\n", path);
            }
            pkg_close(&pkg);
            continue;
        }
        int ok = (is_app || is_patch) && !strncmp(pkg.title_id, "CUSA", 4) && pkg_fake_key(&pkg);
        char tid[10], ver[16] = "";
        uint8_t digest[32];
        snprintf(tid, sizeof(tid), "%s", pkg.title_id);
        memcpy(digest, pkg.digest, sizeof(digest));
        const sfo_item_t *av = ok ? sfo_get(&sfo, "APP_VER") : NULL;
        if (av && !av->is_int)
            snprintf(ver, sizeof(ver), "%s", av->sval);
        pkg_close(&pkg);
        if (!ok) {
            if (first_time(path))
                printf("[scan] skip %s (not a fake PS4 game or update pkg)\n", path);
            continue;
        }
        if (installed_internally(tid)) {
            if (first_time(path))
                printf("[scan] skip %s (%s is installed internally)\n", path, tid);
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/ps4/%s", root, tid);
        mkdirs(dst, 0777);
        snprintf(dst, sizeof(dst), "%s/ps4/%s/%s", root, tid, is_app ? "app.pkg" : "patch.pkg");
        if (exists(dst)) {
            pkg_t cur;
            int same = !pkg_open(&cur, dst) && !memcmp(cur.digest, digest, sizeof(digest));
            if (cur.fd > 0)
                pkg_close(&cur);
            if (same || is_app) {
                /* duplicates, and a second copy of a game, stay where they are */
                if (first_time(path))
                    notify(same ? "%s is already in use for %s; the copy was left in the USB root"
                                : "%s: %s already has a game package; the new one was left in the USB root",
                           e->d_name, tid);
                continue;
            }
            /* a different update: it replaces the current one, which is kept aside */
            char old[PATH_MAX];
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced", root, tid);
            mkdirs(old, 0777);
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced/patch-%ld.pkg", root, tid, (long)time(NULL));
            if (rename(dst, old)) {
                printf("[scan] keep %s aside: %s\n", dst, strerror(errno));
                continue;
            }
            /* the update kept aside keeps its original name */
            char game[PATH_MAX], was[PATH_MAX];
            snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
            if (!origin_find(game, "patch.pkg", was, sizeof(was)))
                origin_add(game, old + strlen(game) + 1, was);
            notify("New update for %s (v%s): the previous one was moved to ps4/%s/replaced", tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            record_origin(root, tid, is_app ? "app.pkg" : "patch.pkg", path);
            moved++;
        }
    }
    }
    closedir(d);
    return moved;
}

/* Configured folders, relative to the selected mounted USB root. */
static void scan_dir(char *out, size_t n, const char *root, int i) {
    if (g_cfg.paths[i][0]) snprintf(out, n, "%s/%s", root, g_cfg.paths[i]);
    else snprintf(out, n, "%s", root);
}

/* Independent progress state for the panel. Updated only on the main daemon
 * thread, even while we pump HTTP events during a long USB search. */
static struct {
    const char *phase;
    char current[16], detail[256], scan_path[256];
    int total, done, ok, failed, folders, pkgs, organized;
} g_progress = {.phase = "idle"};
static int g_operation_busy;
static char g_mount_requested[10];
static int g_seen_usb[10];
static int g_any_usb;
static int g_scan_abort;

static void progress_start(const char *phase, const char *detail) {
    g_progress.phase = phase;
    g_progress.total = g_progress.done = g_progress.ok = g_progress.failed = 0;
    g_progress.folders = g_progress.pkgs = g_progress.organized = 0;
    g_progress.current[0] = g_progress.scan_path[0] = 0;
    snprintf(g_progress.detail, sizeof(g_progress.detail), "%s", detail);
}

/* A single recursive search per explicit click, with bounded depth and no symlinks
 * or other filesystems: searching must never recurse into the system disk. */
static int walk_pkgs(const char *root, const char *folder, int depth, dev_t dev) {
    if (g_scan_abort) return 0;
    if (depth > MAX_WALK_DEPTH || g_progress.folders >= MAX_WALK_DIRS) {
        g_scan_abort = 1;
        return 0;
    }
    struct stat st;
    if (lstat(folder, &st) || !S_ISDIR(st.st_mode) || st.st_dev != dev) return 0;
    g_progress.folders++;
    if ((g_progress.folders % 16) == 1 && !is_mountpoint(root)) {
        g_scan_abort = 1;
        return 0;
    }
    snprintf(g_progress.scan_path, sizeof(g_progress.scan_path), "%s", folder);
    int moved = adopt_dir_pkgs(root, folder);
    g_progress.organized += moved;
    g_progress.pkgs += moved; /* successfully examined; the total is indeterminate */
    pump_events(1);
    DIR *d = opendir(folder);
    if (!d) return moved;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(folder, root) && !strcmp(e->d_name, "ps4")) continue;
        char sub[PATH_MAX];
        int n = snprintf(sub, sizeof(sub), "%s/%s", folder, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(sub)) continue;
        if (lstat(sub, &st)) continue;
        if (S_ISREG(st.st_mode)) {
            size_t len = strlen(e->d_name);
            if (len > 4 && !strcasecmp(e->d_name + len - 4, ".pkg")) g_progress.pkgs++;
        } else if (S_ISDIR(st.st_mode) && st.st_dev == dev) {
            moved += walk_pkgs(root, sub, depth + 1, dev);
        }
        if (g_progress.folders >= MAX_WALK_DIRS) break;
    }
    closedir(d);
    return moved;
}

static void discover_on_drive(const char *root) {
    struct stat st;
    if (!is_mountpoint(root) || stat(root, &st)) return;
    g_lib_sig = lib_signature(root);
    for (int i = 0; i < g_cfg.npaths && !g_scan_abort; i++) {
        char dir[PATH_MAX];
        scan_dir(dir, sizeof(dir), root, i);
        /* Reject configured paths escaping the selected drive via a symlink.
         * lstat on every segment below enforces this for recursive traversal;
         * realpath additionally validates the user-supplied initial directory. */
        char resolved[PATH_MAX];
        size_t base = strlen(root);
        if (!realpath(dir, resolved) || strncmp(resolved, root, base) ||
            (resolved[base] && resolved[base] != '/')) continue;
        walk_pkgs(root, dir, 0, st.st_dev);
    }
}

/* Reconnect restores only titles explicitly activated previously. No recursive
 * PKG search, rename, or registration takes place on a passive USB poll. */
static void restore_known(const char *root) {
    DIR *d = opendir(STATE_DIR);
    if (!d || !is_mountpoint(root)) { if (d) closedir(d); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 16 || strcmp(e->d_name + 9, ".active")) continue;
        char tid[10];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        if (!title_id_valid(tid) || !title_is_active(tid)) continue;
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/ps4/%s/app.pkg", root, tid);
        if (!exists(candidate)) continue;
        if (!db_is_registered(tid) || title_running(tid)) continue;
        title_src_t src = {0};
        if (src_open(&src, root, tid)) { if (src.app.fd >= 0) src_close(&src); continue; }
        /* An equally named game on another HD is not necessarily the same PKG. */
        if (!marker_matches(tid, src.marker) || !hold_ready("app", tid, "app.pkg") ||
            (src.has_patch && !hold_ready("patch", tid, "patch.pkg"))) {
            src_close(&src); continue;
        }
        if (title_mounts_owned(tid, src.dlc_tids, src.ndlc_tids) == 0 &&
            !mount_title(&src) && all_layers_mounted(&src)) {
            manage(&src);
            printf("[usb] restored %s on %s\n", tid, root);
        } else printf("[usb] restore %s failed or foreign layer present\n", tid);
        src_close(&src);
        pump_events(1);
    }
    closedir(d);
}

/* Polling checks only mount metadata. Intensive disk traversal is user-initiated. */
static void scan_all(void) {
    char root[32];
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        int present = is_mountpoint(root);
        count += present;
        if (present && !g_seen_usb[i]) {
            printf("[usb] detected: %s\n", root);
            restore_known(root);
        } else if (!present && g_seen_usb[i]) {
            printf("[usb] disconnected: %s (saved registrations preserved)\n", root);
        }
        g_seen_usb[i] = present;
    }
    g_any_usb = count;
    if (!g_manual_scan) return;
    g_manual_scan = 0;
    if (!count) {
        progress_start("error", "Nenhum HD USB montado. Busca cancelada.");
        notify("Nenhum HD USB montado; busca nao executada");
        return;
    }
    g_operation_busy = 1;
    g_scan_abort = 0;
    progress_start("searching", "Buscando PKGs nas unidades USB montadas");
    for (int i = 0; i < 10; i++) {
        if (!g_seen_usb[i]) continue;
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        discover_on_drive(root);
        if (!is_mountpoint(root)) g_scan_abort = 1;
        if (g_scan_abort) break;
    }
    g_progress.phase = g_scan_abort ? "error" : "complete";
    g_progress.done = g_progress.total = g_progress.ok = g_progress.organized;
    g_progress.failed = g_scan_abort ? 1 : 0;
    snprintf(g_progress.detail, sizeof(g_progress.detail),
             g_scan_abort ? "Busca interrompida: HD indisponivel ou limite de pastas" :
                            "%d pasta(s), %d PKG(s), %d organizado(s)",
             g_progress.folders, g_progress.pkgs, g_progress.organized);
    g_operation_busy = 0;
}

/* --- klog events --- */

static int klog_connect(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(3232);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("[klog] klogsrv\n");
        return s;
    }
    if (s >= 0)
        close(s);
    int fd = open("/dev/klog", O_RDONLY);
    printf("[klog] /dev/klog fd=%d\n", fd);
    return fd;
}

static const char *after(const char *line, const char *tag) {
    const char *p = strstr(line, tag);
    return p ? p + strlen(tag) : NULL;
}

static void end_delete_window(void) {
    g_deleting = 0;
    g_deleting_close_at = 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && title_is_active(g_managed[i].tid))
            mount_paths(g_managed[i].tid, g_managed[i].has_patch, g_managed[i].dlc_tids, g_managed[i].ndlc_tids);
    printf("[delete] window closed, game folders mounted again\n");
}

/* Screens of the two delete flows: the home screen (Options > Delete) and
 * Settings > Storage. From the first of them until focus leaves both flows the game
 * folders are unmounted, so a delete removes /user/app/<TID> cleanly (a mount point
 * cannot be removed). Returns 2 for a screen that opens the window, 1 for a screen
 * that keeps it open (menus and dialogs on the way), 0 otherwise. */
static int delete_scene(const char *name) {
    static const char *opens[] = {"AppDeleteConfirmScene", "console storage delete content", "DeleteDialogScreen", NULL};
    static const char *keeps[] = {"AppDeletingScene", "ReactPopupMenu", "Noname", "Render.NPXS40021",
                                  "storage management", NULL};
    for (int i = 0; opens[i]; i++)
        if (strstr(name, opens[i]))
            return 2;
    for (int i = 0; keeps[i]; i++)
        if (strstr(name, keeps[i]))
            return 1;
    return 0;
}

static void start_delete_window(void) {
    g_deleting_since = time(NULL);
    g_deleting_close_at = 0;
    if (g_deleting)
        return;
    g_deleting = 1;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        if (m->running || title_running(m->tid))
            continue; /* still open: unmounted when it closes (see the End event) */
        unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
    }
    printf("[delete] delete screen open, game folders unmounted\n");
}

/* Copies klog lines about our titles (and crash/mount errors) into the log. */
static void trace_klog(const char *line) {
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", "sqlite",
                                 "SQLITE", "corrupt", "Corrupt", "CORRUPT", "dbr", "DBR", NULL};
    for (int i = 0; keys[i]; i++)
        if (strstr(line, keys[i])) {
            printf("[klog] %.300s\n", line);
            return;
        }
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && strstr(line, g_managed[i].tid) && !strstr(line, "BGLayer") && !strstr(line, "PSM.UI")) {
            printf("[klog] %.300s\n", line);
            return;
        }
}

static void handle_klog_line(const char *line) {
    const char *p;
    char id[40];
    trace_klog(line);
    if ((p = after(line, "OnFocusActiveSceneChanged ["))) {
        /* "[from : type] -> [to : type]": follow the delete screens */
        const char *to = strstr(p, "] -> [");
        if (g_deleting)
            printf("[delete] screen %.200s\n", p);
        if (to) {
            to += 6;
            int kind = delete_scene(to);
            if (kind == 2 || (kind == 1 && g_deleting))
                start_delete_window();
            else if (!kind && g_deleting)
                end_delete_window();
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 1;
            m->running_since = time(NULL);
        }
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
                printf("[delete] %s closed, game folder unmounted\n", m->tid);
            }
        }
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m) {
            char path[64];
            snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
            write_file(path, m->dir, strlen(m->dir), 0666);
            snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
            unlink(path);
            char hold[PATH_MAX];
            hold_path(hold, sizeof(hold), "app", m->tid);
            rm_tree(hold);
            hold_path(hold, sizeof(hold), "patch", m->tid);
            rm_tree(hold);
            notify("%s removed from the library. The pkg stays on the USB drive.", m->name);
            m->used = 0;
        } else if (strlen(id) == 36) {
            /* an add-on of one of our games: keep it out until the drive is reconnected */
            for (int i = 0; i < MAX_TITLES; i++) {
                managed_t *g = &g_managed[i];
                for (int j = 0; g->used && j < g->ndlc_tids; j++) {
                    if (strncmp(id + 7, g->dlc_tids[j], 9))
                        continue;
                    char path[96], hold[PATH_MAX];
                    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", id);
                    write_file(path, id, strlen(id), 0666);
                    snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%.9s/%s", id + 7, id + 20);
                    rm_tree(hold);
                    printf("[delete] add-on %s removed\n", id);
                    i = MAX_TITLES;
                    break;
                }
            }
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

static time_t g_next_scan;
static int g_stop;

static void json_str(buf_t *b, const char *s);

int app_rescan(char *msg, size_t cap) {
    if (g_operation_busy || g_mount_requested[0] || g_manual_scan) {
        snprintf(msg, cap, "Ja existe uma operacao em andamento");
        return -1;
    }
    /* Refresh presence synchronously: a newly plugged drive can be requested
     * before the next 10-second background check. */
    int any = 0;
    for (int i = 0; i < 10; i++) {
        char root[32];
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        any += is_mountpoint(root);
    }
    if (!any) {
        progress_start("error", "Nenhum HD externo montado");
        snprintf(msg, cap, "Nenhum HD USB montado; a busca nao foi iniciada");
        return -1;
    }
    g_manual_scan = 1;
    g_next_scan = 0;
    progress_start("queued", "Busca USB solicitada pelo painel");
    snprintf(msg, cap, "Busca USB agendada");
    return 0;
}

void app_progress_json(buf_t *out) {
    buf_printf(out, "{\"phase\":"); json_str(out, g_progress.phase);
    buf_printf(out, ",\"current\":"); json_str(out, g_progress.current);
    buf_printf(out, ",\"detail\":"); json_str(out, g_progress.detail);
    buf_printf(out, ",\"done\":%d,\"total\":%d,\"ok\":%d,\"failed\":%d",
               g_progress.done, g_progress.total, g_progress.ok, g_progress.failed);
    buf_printf(out, ",\"scan_folders\":%d,\"scan_pkgs\":%d,\"scan_organized\":%d,\"scan_path\":",
               g_progress.folders, g_progress.pkgs, g_progress.organized);
    json_str(out, g_progress.scan_path);
    buf_printf(out, ",\"usb_connected\":%d,\"busy\":%s}", g_any_usb,
               g_operation_busy || g_mount_requested[0] || g_manual_scan ? "true" : "false");
}
void app_stop(void) { g_stop = 1; }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

/* Finds the library folder of tid on any connected storage. */
static int find_root(const char *tid, char *root, size_t n) {
    for (int i = 0; i < 10; i++) {
        char r[32], p[PATH_MAX];
        if (i < 8)
            snprintf(r, sizeof(r), "/mnt/usb%d", i);
        else
            snprintf(r, sizeof(r), "/mnt/ext%d", i - 8);
        snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", r, tid);
        if (is_mountpoint(r) && exists(p)) {
            snprintf(root, n, "%s", r);
            return 0;
        }
    }
    return -1;
}

void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"games\":[");
    int first = 1;
    for (int i = 0; i < 10; i++) {
        char root[32], lib[PATH_MAX];
        if (i < 8)
            snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else
            snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (!is_mountpoint(root))
            continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
                continue;
            title_src_t t;
            char pkg_path[PATH_MAX];
            snprintf(pkg_path, sizeof(pkg_path), "%s/%s/app.pkg", lib, e->d_name);
            if (!exists(pkg_path) || src_open(&t, root, e->d_name))
                continue;
            const char *tid = e->d_name, *state, *mount = "";
            char target[64], from[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", tid);
            int mounted = mounted_from(target, from, sizeof(from));
            if (mounted) {
                struct statfs sf;
                statfs(target, &sf);
                mount = sf.f_flags & MNT_RDONLY ? "ro" : "rw";
            }
            managed_t *m = managed_by(tid, NULL);
            char hold[PATH_MAX];
            int layers = 0;
            hold_path(hold, sizeof(hold), "app", tid);
            int foreign = owned_layers(target, hold, &layers);
            if (installed_internally(tid)) state = "internal";
            else if (served_elsewhere(&t)) state = "duplicate";
            else if ((m && m->running) || title_running(tid)) state = "running";
            else if (foreign) state = "blocked";
            else if (layers && mounted && all_layers_mounted(&t) && db_is_registered(tid)) state = "ready";
            else if (!title_is_active(tid)) state = "inactive";
            else state = "pending";
            buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", tid);
            json_str(out, t.name);
            buf_printf(out, ",\"version\":");
            json_str(out, t.ver);
            buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                       t.has_patch ? "true" : "false",
                       (unsigned long long)(t.app.pkg_size + (t.has_patch ? t.patch.pkg_size : 0)), state, mount);
            json_str(out, t.dir);
            buf_printf(out, ",\"dlc\":%d,\"update_issue\":", t.ndlc);
            json_str(out, t.update_issue);
            buf_printf(out, "}");
            first = 0;
            src_close(&t);
        }
        closedir(d);
    }
    buf_printf(out, "]}");
}

int app_icon(const char *tid, buf_t *out) {
    char p[PATH_MAX], root[32];
    snprintf(p, sizeof(p), "/user/appmeta/%s/icon0.png", tid);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char tmp[65536];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0)
            buf_put(out, tmp, (size_t)n);
        close(fd);
        return out->len ? 0 : -1;
    }
    if (find_root(tid, root, sizeof(root)))
        return -1;
    snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", root, tid);
    pkg_t pkg;
    if (pkg_open(&pkg, p))
        return -1;
    size_t len;
    uint8_t *png = pkg_entry(&pkg, 0x1200, &len);
    pkg_close(&pkg);
    if (!png)
        return -1;
    buf_put(out, png, len);
    free(png);
    return 0;
}

/* Mount and installation are executed on the daemon thread, never inside the
 * HTTP accept callback. The callback only validates and queues one title. */
int app_add(const char *tid, char *msg, size_t cap) {
    char root[32];
    if (!title_id_valid(tid) || find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "Jogo nao encontrado em um HD USB conectado");
        return -1;
    }
    if (installed_internally(tid)) {
        snprintf(msg, cap, "Jogo instalado internamente: operacao bloqueada");
        return -1;
    }
    if (g_operation_busy || g_mount_requested[0] || g_manual_scan) {
        snprintf(msg, cap, "Outra operacao ainda esta em andamento");
        return -1;
    }
    if (title_running(tid)) {
        snprintf(msg, cap, "Jogo em execucao; feche antes de montar");
        return -1;
    }
    snprintf(g_mount_requested, sizeof(g_mount_requested), "%s", tid);
    progress_start("queued", "Montagem solicitada pelo painel");
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    snprintf(msg, cap, "Montagem de %s agendada", tid);
    return 0;
}

static void perform_mount(const char *tid) {
    char root[32];
    g_operation_busy = 1;
    progress_start("activating", "Validando USB e arquivos do jogo");
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    g_progress.total = 4;
    title_src_t src = {0};
    int opened = 0, mounted = 0;
    if (find_root(tid, root, sizeof(root)) || !is_mountpoint(root)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "HD USB desconectado");
        goto fail;
    }
    if (installed_internally(tid) || title_running(tid)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Instalacao interna ou jogo em execucao");
        goto fail;
    }
    if (src_open(&src, root, tid)) {
        if (src.app.fd >= 0) src_close(&src);
        snprintf(g_progress.detail, sizeof(g_progress.detail), "PKG inacessivel ou invalido");
        goto fail;
    }
    opened = 1;
    if (title_mounts_owned(tid, src.dlc_tids, src.ndlc_tids)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Montagem de outro aplicativo no destino");
        goto fail;
    }
    g_progress.done = 1;
    snprintf(g_progress.detail, sizeof(g_progress.detail), "Preparando arquivos auxiliares");
    pump_events(1);
    if (place_files(&src)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Falha preparando arquivos auxiliares");
        goto fail;
    }
    g_progress.done = 2;
    snprintf(g_progress.detail, sizeof(g_progress.detail), "Montando camadas do jogo");
    pump_events(1);
    if (!is_mountpoint(root) || mount_title(&src) || !all_layers_mounted(&src)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Falha na montagem; confira o log");
        goto fail;
    }
    mounted = 1;
    g_progress.done = 3;
    snprintf(g_progress.detail, sizeof(g_progress.detail), "Verificando registro do jogo");
    pump_events(1);
    if (!db_is_registered(tid)) {
        if (g_install_busy) {
            snprintf(g_progress.detail, sizeof(g_progress.detail), "Registro anterior ainda em andamento");
            goto fail;
        }
        if (g_reg_mode == REG_NONE) {
            snprintf(g_progress.detail, sizeof(g_progress.detail), "API de registro indisponivel");
            goto fail;
        }
        char ids[1][10];
        snprintf(ids[0], 10, "%s", tid);
        install_all(ids, 1);
        wait_db_quiet(3, 30);
    }
    if (!db_is_registered(tid) || register_finish(&src)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Registro nao confirmado no banco do sistema");
        goto fail;
    }
    if (set_title_active(tid, 1)) {
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Montado, mas falha ao salvar estado; verifique o log");
        goto fail;
    }
    g_progress.done = g_progress.total;
    g_progress.ok = 1;
    g_progress.phase = "complete";
    snprintf(g_progress.detail, sizeof(g_progress.detail), "%s montado e confirmado", tid);
    notify("PS4Mount: %s montado", tid);
    goto done;
fail:
    g_progress.failed = 1;
    g_progress.phase = "error";
    printf("[manual][mount] %s: %s\n", tid, g_progress.detail);
    /* Do not delete an existing registration, marker, or unknown layer on failure.
     * A previously prepared owned layer can be reconciled by the next click. */
    if (mounted && !db_is_registered(tid) && !g_install_busy) {
        if (!unmount_title(tid, src.dlc_tids, src.ndlc_tids))
            printf("[manual][mount] %s: rollback of unregistered mounted layers OK\n", tid);
        else printf("[manual][mount] %s: unable to roll back owned layers safely\n", tid);
    } else if (mounted) {
        printf("[manual][mount] %s: prepared layer remains; operation incomplete\n", tid);
    }
    notify("PS4Mount: nao foi possivel montar %s; veja o log", tid);
done:
    if (opened) src_close(&src);
    g_operation_busy = 0;
}

/* Desmontar NEVER calls AppUnInstall: the library registration is retained. */
int app_remove(const char *tid, char *msg, size_t cap) {
    if (!title_id_valid(tid) || g_operation_busy || g_mount_requested[0] || g_manual_scan) {
        snprintf(msg, cap, "ID invalido ou outra operacao em andamento");
        return -1;
    }
    if (title_running(tid)) {
        snprintf(msg, cap, "Feche o jogo antes de desmontar");
        return -1;
    }
    char root[32];
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "HD do jogo desconectado; registro preservado");
        return -1;
    }
    title_src_t src = {0};
    if (src_open(&src, root, tid)) {
        if (src.app.fd >= 0) src_close(&src);
        snprintf(msg, cap, "Nao foi possivel ler os metadados do jogo");
        return -1;
    }
    g_operation_busy = 1;
    progress_start("activating", "Desmontando camadas controladas pelo PS4Mount");
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    g_progress.total = 2;
    int rc = title_mounts_owned(tid, src.dlc_tids, src.ndlc_tids) ||
             unmount_title(tid, src.dlc_tids, src.ndlc_tids);
    g_progress.done = 1;
    if (!rc) rc = set_title_active(tid, 0);
    src_close(&src);
    if (rc) {
        snprintf(msg, cap, "Montagem externa/conflitante ou falha de desmontagem; nada foi desinstalado");
        g_progress.phase = "error";
        g_progress.failed = 1;
        snprintf(g_progress.detail, sizeof(g_progress.detail), "%s", msg);
        printf("[manual][unmount] %s: %s\n", tid, msg);
    } else {
        snprintf(msg, cap, "%s desmontado; registro na biblioteca preservado", tid);
        g_progress.done = 2;
        g_progress.ok = 1;
        g_progress.phase = "complete";
        snprintf(g_progress.detail, sizeof(g_progress.detail), "%s", msg);
    }
    g_operation_busy = 0;
    return rc ? -1 : 0;
}

/* Manual mounting deliberately excludes the old PKG restore/uninstall workflow.
 * The original restore action called app_remove(), which now only unmounts and
 * therefore would leave installed DB rows pointing at moved-away packages. */
int app_restore(const char *tid, char *msg, size_t cap) {
    (void)tid;
    snprintf(msg, cap, "Restaurar PKGs desativado na base manual: o fluxo original desinstalava o jogo");
    return -1;
}
int app_restored_forget(char *msg, size_t cap) {
    snprintf(msg, cap, "Restaurar PKGs desativado na base manual");
    return -1;
}

/* --- settings --- */

/* A scan folder: relative to the drive, "" for the root. Folders inside ps4/ (where
 * PS4Mount keeps the games) and anything leaving the drive are refused. */
static int clean_scan_path(const char *in, char *out, size_t n, char *why, size_t wn) {
    while (*in == ' ' || *in == '/')
        in++;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '/' || out[l - 1] == '\r'))
        out[--l] = 0;
    if (l >= 120) {
        snprintf(why, wn, "folder name too long: %.40s...", out);
        return -1;
    }
    if (strchr(out, '\\') || strchr(out, ':') || strstr(out, "//")) {
        snprintf(why, wn, "invalid folder: %s", out);
        return -1;
    }
    for (const char *c = out; *c; c = strchr(c, '/') ? strchr(c, '/') + 1 : c + strlen(c))
        if (!strncmp(c, "..", 2) && (c[2] == '/' || !c[2])) {
            snprintf(why, wn, "folders outside the drive are not allowed: %s", out);
            return -1;
        }
    if (!strncasecmp(out, "ps4", 3) && (!out[3] || out[3] == '/')) {
        snprintf(why, wn, "the ps4 folder holds the games PS4Mount already moved; choose another folder");
        return -1;
    }
    return 0;
}

static void config_save(void) {
    buf_t b = {0};
    buf_printf(&b, "batch=%d\nbatch_size=%d\ndlc=%d\n", g_cfg.batch, g_cfg.batch_size, g_cfg.dlc);
    for (int i = 0; i < g_cfg.npaths; i++)
        buf_printf(&b, "path=%s\n", g_cfg.paths[i]);
    write_file(CONFIG_PATH, b.data, b.len, 0666);
    buf_free(&b);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    config_t c = g_cfg;
    c.npaths = 0;
    char line[256], why[160], path[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "batch=", 6))
            c.batch = atoi(line + 6) != 0;
        else if (!strncmp(line, "batch_size=", 11))
            c.batch_size = atoi(line + 11);
        else if (!strncmp(line, "dlc=", 4))
            c.dlc = atoi(line + 4) != 0;
        else if (!strncmp(line, "path=", 5) && c.npaths < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 5, path, sizeof(path), why, sizeof(why)))
            snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
    }
    fclose(f);
    if (c.batch_size < 1 || c.batch_size > 50)
        c.batch_size = 5;
    if (!c.npaths)
        c.npaths = 1; /* the root */
    g_cfg = c;
    printf("[config] batch=%d batch_size=%d dlc=%d folders=%d\n", g_cfg.batch, g_cfg.batch_size, g_cfg.dlc,
           g_cfg.npaths);
}

void app_config_json(buf_t *out) {
    buf_printf(out, "{\"batch\":%s,\"batch_size\":%d,\"dlc\":%s,\"paths\":[", g_cfg.batch ? "true" : "false",
               g_cfg.batch_size, g_cfg.dlc ? "true" : "false");
    for (int i = 0; i < g_cfg.npaths; i++) {
        char shown[130];
        snprintf(shown, sizeof(shown), "/%s", g_cfg.paths[i]);
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, shown);
    }
    buf_printf(out, "]}");
}

static void url_decode(const char *in, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (in[i] == '+')
            out[o++] = ' ';
        else if (in[i] == '%' && i + 2 < len) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else
            out[o++] = in[i];
    }
    out[o] = 0;
}

/* body: batch=0|1&batch_size=N&dlc=0|1&paths=<folders, one per line> (form encoded). */
int app_config_set(const char *body, char *msg, size_t cap) {
    if (g_operation_busy || g_manual_scan || g_mount_requested[0]) {
        snprintf(msg, cap, "Configuracoes bloqueadas durante operacao");
        return -1;
    }
    config_t c = g_cfg;
    char value[1200], why[160] = "";
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + len) {
            url_decode(eq + 1, (size_t)(p + len - eq - 1), value, sizeof(value));
            size_t k = (size_t)(eq - p);
            if (k == 5 && !strncmp(p, "batch", 5))
                c.batch = atoi(value) != 0;
            else if (k == 10 && !strncmp(p, "batch_size", 10))
                c.batch_size = atoi(value);
            else if (k == 3 && !strncmp(p, "dlc", 3))
                c.dlc = atoi(value) != 0;
            else if (k == 5 && !strncmp(p, "paths", 5)) {
                c.npaths = 0;
                for (char *line = strtok(value, "\n"); line; line = strtok(NULL, "\n")) {
                    char path[128];
                    if (clean_scan_path(line, path, sizeof(path), why, sizeof(why))) {
                        snprintf(msg, cap, "Not saved: %s", why);
                        return -1;
                    }
                    int dup = 0;
                    for (int i = 0; i < c.npaths; i++)
                        dup |= !strcmp(c.paths[i], path);
                    if (dup)
                        continue;
                    if (c.npaths == MAX_SCAN_PATHS) {
                        snprintf(msg, cap, "Not saved: at most %d folders", MAX_SCAN_PATHS);
                        return -1;
                    }
                    snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
                }
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    if (c.batch_size < 1 || c.batch_size > 50) {
        snprintf(msg, cap, "Not saved: games per group must be between 1 and 50");
        return -1;
    }
    if (!c.npaths) {
        snprintf(msg, cap, "Not saved: add at least one folder (/ is the drive root)");
        return -1;
    }
    g_cfg = c;
    config_save();
    printf("[config] saved: batch=%d batch_size=%d dlc=%d folders=%d\n", c.batch, c.batch_size, c.dlc, c.npaths);
    g_next_scan = 0;
    snprintf(msg, cap, "Settings saved");
    return 0;
}

/* --- Media tab shortcut (category 65536 + deeplinkUri, as other PS5 web panels) --- */

#define PANEL_ID "PMNT00001"
static const char k_panel_param[] =
    "{\n"
    "    \"titleId\": \"" PANEL_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:8770/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"PS4Mount\"\n"
    "        }\n"
    "    }\n"
    "}\n";

static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t *tmp = malloc(size);
    int same = tmp && read(fd, tmp, size) == (ssize_t)size && !memcmp(tmp, data, size);
    free(tmp);
    close(fd);
    return same;
}

/* Installs the shortcut when it is missing or out of date (deleting it from the
 * home screen makes the next start install it again). */
static void install_panel_shortcut(void) {
    const char *param = "/user/app/" PANEL_ID "/sce_sys/param.json", *icon = "/user/app/" PANEL_ID "/sce_sys/icon0.png";
    /* sceAppInstUtilAppExists reports 0 for this shortcut on fw 13.20; app.db is reliable */
    int present = db_is_registered(PANEL_ID), rc = 0;
    int same_param = same_file(param, k_panel_param, sizeof(k_panel_param) - 1);
    int same_icon = same_file(icon, icon0_png, sizeof(icon0_png));
    printf("[panel] exists rc=0x%08x present=%d param=%d icon=%d\n", rc, present, same_param, same_icon);
    if (!rc && present && same_param && same_icon)
        return;
    mkdirs("/user/app/" PANEL_ID "/sce_sys", 0755);
    if (write_file(param, k_panel_param, sizeof(k_panel_param) - 1, 0644) ||
        write_file(icon, icon0_png, sizeof(icon0_png), 0644)) {
        printf("[panel] shortcut files: %s\n", strerror(errno));
        return;
    }
    rc = register_call(PANEL_ID);
    printf("[panel] shortcut install rc=0x%08x\n", rc);
    if (!rc)
        notify("PS4Mount added to the Media tab");
}

static int single_instance(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOCK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

/* Rest mode closes the network sockets of payloads: a listening socket that stopped
 * listening is found here so it can be opened again. */
static int still_listening(int s) {
    int on = 0;
    socklen_t n = sizeof(on);
    return s >= 0 && !getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &on, &n) && on;
}

static int g_klog = -1, g_web = -1;
static int g_live; /* klogsrv replays its backlog first: live after the first quiet 100 ms */

/* Serves the panel and handles klog lines for up to ms milliseconds. */
static void pump_events(int ms) {
    static char buf[16384], line[4096];
    static size_t ll;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000;
    for (;;) {
        if (g_klog < 0) {
            g_klog = klog_connect();
            g_live = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_nsec - now.tv_nsec) / 1000000;
        if (left <= 0)
            return;
        long wait = g_live || g_klog < 0 ? left : (left < 100 ? left : 100);
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = -1;
        if (g_klog >= 0) {
            FD_SET(g_klog, &fs);
            maxfd = g_klog;
        }
        if (g_web >= 0) {
            FD_SET(g_web, &fs);
            if (g_web > maxfd)
                maxfd = g_web;
        }
        if (maxfd < 0) {
            usleep((useconds_t)left * 1000);
            return;
        }
        int ready = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (ready == 0 && !g_live && g_klog >= 0) {
            g_live = 1;
            printf("[klog] backlog done, following live\n");
        }
        if (ready <= 0)
            continue;
        if (g_web >= 0 && FD_ISSET(g_web, &fs))
            web_serve(g_web);
        if (g_klog < 0 || !FD_ISSET(g_klog, &fs))
            continue;
        ssize_t n = read(g_klog, buf, sizeof(buf));
        if (n <= 0) {
            close(g_klog);
            g_klog = -1;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (ll < sizeof(line) - 1)
                    line[ll++] = buf[i];
                continue;
            }
            line[ll] = 0;
            ll = 0;
            if (g_live)
                handle_klog_line(line);
        }
    }
}

int main(void) {
    int lock = single_instance();
    if (lock < 0) {
        printf(VERSION " already running\n");
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    mkdirs(STATE_DIR, 0777);
    freopen(STATE_DIR "/log.txt", "a", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    config_load();
    signal(SIGPIPE, SIG_IGN);
    check_firmware();
    if (g_reg_mode != REG_NONE)
        install_panel_shortcut(); /* registered by the system like the games */
    g_web = web_listen();
    if (g_reg_mode == REG_ALL)
        notify(g_web >= 0 ? VERSION " running. Panel in the Media tab or http://<PS5 IP>:8770"
                          : VERSION " running (panel unavailable)");
    else if (g_reg_mode == REG_TITLEDIR)
        notify(VERSION " running (firmware %s: games are added one at a time). Panel in the Media tab or "
                       "http://<PS5 IP>:8770",
               g_fw_text);
    else
        notify(VERSION ": firmware %s is not supported yet. To protect the system database, no games are added.",
               g_fw_text);
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            notify(VERSION " stopped");
            return 0;
        }
        if (!still_listening(lock)) {
            /* lost in rest mode: take it again, or stop if another PS4Mount has it now */
            close(lock);
            if ((lock = single_instance()) < 0) {
                notify(VERSION " stopped: another PS4Mount is running");
                return 0;
            }
            printf("[main] instance lock opened again\n");
        }
        if (!still_listening(g_web)) {
            int was = g_web >= 0;
            if (g_web >= 0)
                close(g_web);
            g_web = web_listen();
            if (was || g_web >= 0) /* log changes only, not every retry */
                printf("[main] web panel %s\n", g_web >= 0 ? "listening again" : "unavailable");
        }
        if (g_deleting && g_deleting_close_at && time(NULL) >= g_deleting_close_at)
            end_delete_window();
        if (g_deleting && time(NULL) - g_deleting_since > 600)
            end_delete_window(); /* the line of the screen change was missed */
        if (g_mount_requested[0] && !g_deleting && !g_operation_busy) {
            char tid[10];
            snprintf(tid, sizeof(tid), "%s", g_mount_requested);
            g_mount_requested[0] = 0;
            perform_mount(tid);
            continue;
        }
        if (time(NULL) >= g_next_scan && !g_deleting && !g_operation_busy) {
            scan_all(); /* USB metadata always; full discovery only on explicit click */
            g_next_scan = time(NULL) + SCAN_SECONDS;
        }
        pump_events(1000);
    }
}
