/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount: manual discovery/activation, persisted mount restoration, web panel. */
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
#include "crypto.h"
#include "db.h"
#include "pkg.h"
#include "scan_walk.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.4.22-preserve-dlcs"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10 /* legacy value; polling for PKGs is disabled */
#define STATE_DIR "/data/ps4mount"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/ps4mount"
#define MAX_TITLES 256
#define MAX_DLC 64
#define BATCH_QUIET 10 /* seconds without writes to the system databases between groups */
#define MAX_DLC_TIDS MAX_DLC

#define MAX_SCAN_PATHS 8
#define CONFIG_PATH STATE_DIR "/config.txt"

/* Settings, kept in CONFIG_PATH and edited in the panel. */
typedef struct {
    int batch;      /* new games are added in groups */
    int batch_size; /* games per group */
    int dlc;        /* legacy config field; DLCs are always included */
    char paths[MAX_SCAN_PATHS][128]; /* folders searched for pkgs, relative to each drive; "" is the root */
    int npaths;
} config_t;

static config_t g_cfg = {1, 5, 1, {""}, 1};

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceAppInstUtilInitialize(void);

#include "ps5_appinst_pkg.h"
#include "uninstall_event.h"
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
/* Targeted registration only. No fallback to global AppInstallAll: a prior full-library
 * attempt caused a console panic. Resolve BEFORE writing any title metadata. */
typedef int (*app_install_title_dir_fn)(const char *, const char *, void *);
static app_install_title_dir_fn one_title_install_api(void) {
    uint32_t handle = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
        return NULL;
    return (app_install_title_dir_fn)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
}


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

/* Do not interpret an empty /mnt/usbN folder as a connected drive.
 * Check the live mount table and filesystem identity; no game package is read. */
static int is_mountpoint(const char *path) {
    struct statfs sf;
    struct stat st;
    if (statfs(path, &sf) || strcmp(sf.f_mntonname, path) ||
        !strcmp(sf.f_fstypename, "nullfs") || stat(path, &st) || !S_ISDIR(st.st_mode))
        return 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0) return 0;
    struct statfs *list = calloc((size_t)n + 16, sizeof(*list));
    if (!list) return 0;
    n = getfsstat(list, (long)((size_t)(n + 16) * sizeof(*list)), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path) &&
            !strcmp(list[i].f_fstypename, sf.f_fstypename) &&
            !strcmp(list[i].f_mntfromname, sf.f_mntfromname)) { found = 1; break; }
    free(list);
    return found;
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
            /* USB PKG is the source of truth. Legacy .removed flags and DB
             * registration state must never hide valid DLCs from Mount. */
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
    /* Mount always discovers the complete bundle, even if legacy config had
     * dlc=0 or a previous PS5 uninstall left stale .removed flags. */
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
static int dlc_ready(const title_src_t *t) {
    char p[PATH_MAX], actual[PATH_MAX];
    for (int i = 0; i < t->ndlc; i++) {
        snprintf(p, sizeof(p), HOLD_DIR "/addcont/%s/%s/ac.pkg", t->dlc[i].pkg.title_id,
                 t->dlc[i].pkg.content_id + 20);
        ssize_t n = readlink(p, actual, sizeof(actual) - 1);
        if (n <= 0) return 0;
        actual[n] = 0;
        if (strcmp(actual, t->dlc[i].path) || !exists(p)) return 0;
    }
    return 1;
}

static void progress_set(const char *phase, const char *detail);
static void pump_events(int ms);

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
    int group_size = g_cfg.batch ? g_cfg.batch_size : t->ndlc;
    if (group_size < 1) group_size = 5;
    for (int i = 0; i < t->ndlc; i++) {
        if (i % group_size == 0)
            printf("[manual][dlc] %s preparing group %d/%d (%d DLC total)\n", tid,
                   i / group_size + 1, (t->ndlc + group_size - 1) / group_size, t->ndlc);
        char detail[96];
        snprintf(detail, sizeof(detail), "Preparando DLC %d/%d", i + 1, t->ndlc);
        progress_set("activating", detail);
        rc |= place_dlc(&t->dlc[i]);
        pump_events(1);
    }

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

/* Reject hidden foreign mount layers, not just the currently visible top layer.
 * All layers on a target must originate from the same expected PS4Mount directory. */
static int owned_layer_count(const char *target, const char *hold) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0) return -1;
    struct statfs *list = calloc((size_t)n + 16, sizeof(*list));
    if (!list) return -1;
    n = getfsstat(list, (long)((size_t)(n + 16) * sizeof(*list)), MNT_NOWAIT);
    if (n < 0) { free(list); return -1; }
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].f_mntonname, target)) continue;
        if (strcmp(list[i].f_fstypename, "nullfs") ||
            strcmp(list[i].f_mntfromname, hold)) {
            free(list);
            return -1;
        }
        count++;
    }
    free(list);
    return count;
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

/* The console's delete-screen handler releases only app/patch here; the
 * explicit Desmontar button uses release_title_bundle to also release DLCs.
 * Never force-remove unknown layers, especially after a USB disconnect. */
static void unmount_title(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    for (int i = 0; i < 2; i++) {
        const char *kind = i ? "patch" : "app";
        snprintf(target, sizeof(target), "/user/%s/%s", kind, tid);
        hold_path(hold, sizeof(hold), kind, tid);
        if (dead_layer(target)) {
            printf("[mount] %s unreadable; no forced release\n", target);
            continue;
        }
        if (top_layer(target, hold) == 1 && unmount(target, 0))
            printf("[mount] %s release failed: %s\n", target, strerror(errno));
    }
    /* These may be shared by another game's DLCs: leave them in place.
     * They will be safely reused or refused on the next mount attempt. */
    (void)dtids;
    (void)ndtids;
}

static int mount_dir(const char *dir, const char *target) {
    char parent[64];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir(parent, 0770) == 0)
            printf("[mount] created %s\n", parent);
    }
    if (dead_layer(target)) {
        printf("[manual][mount] %s unreadable; leaving layer untouched\n", target);
        return -1;
    }
    int count = owned_layer_count(target, dir);
    if (count < 0) return -1;
    int top = top_layer(target, dir);
    if (top == 1 && count > 0) return 0;
    if (top != 0 || count > 0) {
        printf("[manual][mount] %s occupied by foreign layer; refusing\n", target);
        return -1;
    }
    mkdir(target, 0777);
    if (nullfs_mount(dir, target)) return -1;
    return top_layer(target, dir) == 1 ? 0 : -1;
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
/* Mount one complete title group: app, patch and all external DLC directories.
 * New layers only are rolled back on failure. Existing layers are never removed
 * by this rollback, and normal internal add-on installations stay untouched. */
static int mount_paths(const char *tid, int has_patch, char (*dtids)[10], int ndtids) {
    struct { char target[64]; char hold[128]; int added; } acquired[2 + MAX_DLC_TIDS];
    int used = 0;
    for (int i = 0; i < 2 + ndtids; ++i) {
        const char *kind = i == 0 ? "app" : (i == 1 ? "patch" : "addcont");
        if (i == 1 && !has_patch) continue;
        const char *id = i < 2 ? tid : dtids[i - 2];
        if (i >= 2 && addcont_installed(id)) {
            printf("[bundle][mount] %s: /user/addcont/%s already holds internal add-ons; preserving\n", tid, id);
            continue;
        }
        if (used >= (int)(sizeof(acquired) / sizeof(acquired[0]))) return -1;
        snprintf(acquired[used].target, sizeof(acquired[used].target), "/user/%s/%s", kind, id);
        hold_path(acquired[used].hold, sizeof(acquired[used].hold), kind, id);
        int before = owned_layer_count(acquired[used].target, acquired[used].hold);
        if (before < 0 || mount_dir(acquired[used].hold, acquired[used].target)) goto rollback;
        acquired[used].added = before == 0;
        used++;
    }
    printf("[bundle][mount] %s: app%s, %d add-on title folder(s) checked\n", tid,
           has_patch ? "+patch" : "", ndtids);
    return 0;
rollback:
    printf("[bundle][mount] %s: group incomplete, rolling back newly mounted layers\n", tid);
    for (int j = used - 1; j >= 0; --j) {
        if (!acquired[j].added) continue;
        if (top_layer(acquired[j].target, acquired[j].hold) == 1 &&
            owned_layer_count(acquired[j].target, acquired[j].hold) == 1 &&
            unmount(acquired[j].target, 0))
            printf("[bundle][mount] rollback %s failed: %s\n", acquired[j].target, strerror(errno));
    }
    return -1;
}

static int mount_title(title_src_t *t) {
    return mount_paths(t->app.title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

/* Confirm base, update and expected owned DLC mounts, not only syscall success. */
static int title_layers_mounted(const title_src_t *t) {
    char target[64], hold[PATH_MAX];
    const char *tid = t->app.title_id;
    for (int i = 0; i < 2 + t->ndlc_tids; i++) {
        const char *kind = i == 0 ? "app" : (i == 1 ? "patch" : "addcont");
        if (i == 1 && !t->has_patch) continue;
        const char *id = i < 2 ? tid : t->dlc_tids[i - 2];
        if (i >= 2 && addcont_installed(id)) continue;
        snprintf(target, sizeof(target), "/user/%s/%s", kind, id);
        hold_path(hold, sizeof(hold), kind, id);
        if (owned_layer_count(target, hold) < 1 || top_layer(target, hold) != 1)
            return 0;
    }
    return 1;
}

/* Check ALL target paths before writing metadata. Fail closed if any mount is
 * unreadable or belongs to another program; do not unmount it. */
static int target_mounts_safe(const title_src_t *t) {
    char target[64], hold[PATH_MAX];
    for (int i = 0; i < 2 + t->ndlc_tids; i++) {
        const char *kind = i == 0 ? "app" : (i == 1 ? "patch" : "addcont");
        if (i == 1 && !t->has_patch) continue;
        const char *tid = i < 2 ? t->app.title_id : t->dlc_tids[i - 2];
        if (i >= 2 && addcont_installed(tid)) continue;
        snprintf(target, sizeof(target), "/user/%s/%s", kind, tid);
        hold_path(hold, sizeof(hold), kind, tid);
        if (dead_layer(target) || owned_layer_count(target, hold) < 0 ||
            top_layer(target, hold) < 0) {
            printf("[manual][preflight] foreign or unreadable mount %s; no metadata written\n", target);
            return -1;
        }
    }
    return 0;
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


static int removed(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    return exists(p);
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

static int title_state(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (installed_internally(tid) || removed(tid))
        return -1;
    if (served_elsewhere(t)) {
        if (first_time(t->app_path))
            printf("[scan] %s: %s is already served from another drive, this copy is not used\n", t->dir, tid);
        return -1;
    }
    char pdb[PATH_MAX];
    snprintf(pdb, sizeof(pdb), "/user/playgo/app/%s/00/d0.pdb", t->app.content_id);
    /* folders set up by 0.3.1 and older lack the internal folder: registered again */
    return !db_is_registered(tid) || !exists(pdb) || !marker_matches(tid, t->marker) ||
           !hold_ready("app", tid, "app.pkg") || (t->has_patch && !hold_ready("patch", tid, "patch.pkg")) ||
           !dlc_ready(t);
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

/* Stage one selected game. No global registration path exists. */
static int register_prepare(title_src_t *t) {
    if (place_files(t) || mount_title(t))
        return -1;
    manage(t);
    return 0;
}

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
static int waiting_count(const char *root) {
    size_t l = strlen(root);
    int n = 0;
    for (size_t i = 0; i < g_nwaiting; i++)
        n += !strncmp(g_waiting[i].path, root, l) && g_waiting[i].path[l] == '/' && exists(g_waiting[i].path);
    return n;
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
    return 1;
}

/* Moves PS4 game (gd) and update (gp) pkgs found in dir (the drive root or a scan
 * folder) into <root>/ps4/<TID>/app.pkg and patch.pkg, then add-ons (ac) next to
 * their game. */
/* Progress of explicit recursive search. Counts are observed, not guessed:
 * total directory/file count is unknown until the USB traversal completes. */
static int g_scan_folders, g_scan_examined, g_scan_organized;
static int g_scan_abort;
static int g_stop;
static char g_scan_location[PATH_MAX];
static void scan_progress_folder(const char *path, void *ctx) {
    const char *root = ctx;
    g_scan_folders++;
    snprintf(g_scan_location, sizeof(g_scan_location), "%s", path);
    if (g_stop || ((g_scan_folders % 8) == 1 && !is_mountpoint(root))) {
        g_scan_abort = 1;
        return;
    }
    /* Serve /api/progress even inside long, deeply nested USB traversals. */
    if (!(g_scan_folders % 8)) pump_events(1);
}

static int adopt_dir_pkgs(const char *root, const char *dir) {
    int moved = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    for (int pass = 0; pass < 2 && !g_scan_abort; pass++, rewinddir(d)) {
    if (pass == 1)
        g_lib_sig = lib_signature(root); /* games moved in pass 0 count */
    while ((e = readdir(d))) {
        if (g_scan_abort || g_stop || (g_scan_examined && !(g_scan_examined % 16) && !is_mountpoint(root))) {
            g_scan_abort = 1;
            break;
        }
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".pkg"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (pass == 0) {
            g_scan_examined++;
            pump_events(1);
        }
        waiting_t *w = waiting_find(path);
        if (w && w->sig == g_lib_sig)
            continue; /* waiting add-on, and no new game since the last check */
        pkg_t pkg;
        if (pkg_open(&pkg, path))
            continue;
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        int is_app = cat && !strcmp(cat->sval, "gd"), is_patch = cat && !strcmp(cat->sval, "gp");
        int is_dlc = cat && !strcmp(cat->sval, "ac");
        if (is_dlc || pass == 1) {
            /* add-ons go in the second pass: their game may be in this folder too */
            if (pass == 1 && is_dlc && !strncmp(pkg.title_id, "CUSA", 4)) {
                if (pkg_fake_key(&pkg)) {
                    int count = adopt_dlc(root, e->d_name, path, &pkg);
                    moved += count;
                    g_scan_organized += count;
                } else if (first_time(path))
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
            notify("New update for %s (v%s): the previous one was moved to ps4/%s/replaced", tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            moved++;
            g_scan_organized++;
        }
    }
    }
    closedir(d);
    return moved;
}

typedef struct {
    char tid[10], name[128];
    int state, ndlc, has_patch;
} title_t;

/* A ps4/ folder or a .pkg anywhere inside a configured search folder.
 * Other partitions (like Ventoy boot partitions) are skipped if empty. */
static void scan_dir(char *out, size_t n, const char *root, int i) {
    if (g_cfg.paths[i][0])
        snprintf(out, n, "%s/%s", root, g_cfg.paths[i]);
    else
        snprintf(out, n, "%s", root);
}

/* Walk subfolders for packages; the visitor leaves every original pkg untouched. */
static int scan_should_stop(void *context) {
    const char *root = context;
    if (!g_scan_abort && (g_stop || ((g_scan_folders % 8) == 1 && !is_mountpoint(root))))
        g_scan_abort = 1;
    return g_scan_abort;
}

static int adopt_pkg_dir(const char *dir, void *context) {
    if (scan_should_stop(context)) return 0;
    int moved = adopt_dir_pkgs((const char *)context, dir);
    pump_events(1); /* publish per-folder results, not only per drive */
    return moved;
}


static int adopt_root_pkgs(const char *root) {
    char dir[PATH_MAX], lib[PATH_MAX];
    int moved = 0;
    g_dlc_orphans = g_dlc_dupes = 0;
    g_lib_sig = lib_signature(root);
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(dir, sizeof(dir), root, i);
        if (g_scan_abort) break;
        moved += walk_pkg_dirs_progress_cancel(dir, lib, adopt_pkg_dir, (void *)root, 0,
                                              scan_progress_folder, (void *)root,
                                              scan_should_stop, (void *)root);
    }
    if (g_dlc_orphans)
        notify("%s: %d add-on(s) wait for their game, which is not on this drive (see the log)", root + 5,
               waiting_count(root));
    if (g_dlc_dupes)
        notify("%s: %d add-on copy(ies) were left in place because the game already has them", root + 5, g_dlc_dupes);
    return moved;
}

/* The upstream automatic scan_root/scan_all/registration loop is deliberately
 * removed. Manual discovery only calls adopt_root_pkgs() on explicit POST. */

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
        if (g_managed[i].used)
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
        /* The old handler treated even 0x80020042 failures as a successful
         * deletion, hiding DLCs from future scans. Require an explicit 0. */
        unsigned int uninstall_rc = ~0u;
        (void)ps4mount_uninstall_result(p, &uninstall_rc);
        if (uninstall_rc != 0) {
            printf("[delete] %s uninstall not confirmed rc=0x%08x; preserving USB bundle\n",
                   id, uninstall_rc);
            if (g_deleting) g_deleting_close_at = time(NULL) + 3;
            return;
        }
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
        } else if (strlen(id) == 36 && g_deleting) {
            /* v0.5.0: avoid treating an internal add-on event as user delete.
             * Do not erase the held USB add-on or create a .removed flag.
             * A future explicit Mount must be able to rebuild this DLC. */
            printf("[delete] add-on %s uninstalled by PS5; USB source stays eligible for Mount\n", id);
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

/* Jobs are accepted by POST and executed by the main loop, never from GET. */
static int g_discover_requested;
static char g_activate_request[10];
static char g_dlc_register_request[10];
static char g_dlc_install_request[10];
static char g_dlc_install_only_cid[37];
/* Independent one-DLC/one-method experiment, processed in main loop. */

static int g_restore_running;
typedef struct {
    const char *phase;
    int total, done, ok, failed;
    char current[10];
    char detail[192];
} progress_t;
static progress_t g_progress = {"idle", 0, 0, 0, 0, "", "Waiting for action"};
static int g_dlc_percent = -1;    /* -1 means indeterminate */
static char g_dlc_content[48];
static int g_appinst_ready = 0;
static void register_dlcs_queued(void);
static void install_dlcs_queued(void);
static int redirect_mounted(void);
static int redirect_locked(void);
static int pilot_locked(void);
static int pilot_restore_internal(int recover);

static void progress_set(const char *phase, const char *detail) {
    g_progress.phase = phase;
    snprintf(g_progress.detail, sizeof(g_progress.detail), "%s", detail);
}

static int any_external_usb(void);

int app_rescan(void) {
    if (redirect_locked()) {
        printf("[dlc-redirect] search refused while temporary native overlay active\n");
        return -1;
    }
    if (g_discover_requested || g_restore_running || g_activate_request[0] || g_dlc_register_request[0] || g_dlc_install_request[0] ||
        !strcmp(g_progress.phase, "searching") || !strcmp(g_progress.phase, "activating") ||
        !strcmp(g_progress.phase, "registering_dlcs") || !strcmp(g_progress.phase, "dlc_register_queued"))
        return -1;
    if (!any_external_usb()) {
        g_progress = (progress_t){"error", 0, 0, 0, 0, "", "No external USB mounted; search not started"};
        printf("[manual][scan] rejected: no real USB mount\n");
        return -1;
    }
    g_discover_requested = 1;
    g_scan_folders = g_scan_examined = g_scan_organized = 0;
    g_scan_location[0] = 0;
    progress_set("searching", "Search queued; waiting for USB discovery to start");
    printf("[manual] USB discovery requested from panel\n");
    return 0;
}

void app_stop(void) {
    if (redirect_mounted() && !title_running("CUSA03842")) {
        char msg[256];
        app_restore_native_dlc("CUSA03842", msg, sizeof(msg));
        printf("[dlc-redirect] stop cleanup: %s\n", msg);
    }
    g_stop = 1;
}

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

/* Cache immutable package metadata; the panel polls state/progress but must not
 * re-open hundreds of PKGs or traverse the whole drive every few seconds. */
#define CATALOG_MAX (MAX_TITLES * 2)
typedef struct {
    char tid[10], name[128], ver[8], dir[PATH_MAX], issue[256];
    uint64_t size;
    int patch, dlc;
} catalog_item_t;
static catalog_item_t g_catalog[CATALOG_MAX];
static int g_catalog_count, g_catalog_dirty = 1;
static int active_status(const char *tid);

static void catalog_refresh(void) {
    if (!g_catalog_dirty || g_restore_running) return;
    g_catalog_count = 0;
    for (int i = 0; i < 10; ++i) {
        char root[32], lib[PATH_MAX];
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (!is_mountpoint(root)) continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && g_catalog_count < CATALOG_MAX) {
            if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4)) continue;
            char pkg_path[PATH_MAX];
            snprintf(pkg_path, sizeof(pkg_path), "%s/%s/app.pkg", lib, e->d_name);
            if (!exists(pkg_path)) continue;
            int duplicate = 0;
            for (int j = 0; j < g_catalog_count; j++)
                duplicate |= !strcmp(g_catalog[j].tid, e->d_name);
            if (duplicate) {
                printf("[manual][catalog] duplicate %s on %s, using first USB library\n", e->d_name, root);
                continue;
            }
            title_src_t t = {0};
            if (src_open(&t, root, e->d_name)) continue;
            catalog_item_t *item = &g_catalog[g_catalog_count++];
            memset(item, 0, sizeof(*item));
            snprintf(item->tid, sizeof(item->tid), "%s", e->d_name);
            snprintf(item->name, sizeof(item->name), "%s", t.name);
            snprintf(item->ver, sizeof(item->ver), "%s", t.ver);
            snprintf(item->dir, sizeof(item->dir), "%s", t.dir);
            snprintf(item->issue, sizeof(item->issue), "%s", t.update_issue);
            item->patch = t.has_patch;
            item->dlc = t.ndlc;
            item->size = t.app.pkg_size + (t.has_patch ? t.patch.pkg_size : 0);
            src_close(&t);
        }
        closedir(d);
    }
    g_catalog_dirty = 0;
    printf("[manual][catalog] cached %d organized game(s) (no recursive discovery)\n", g_catalog_count);
}

void app_games_json(buf_t *out) {
    catalog_refresh();
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"games\":[");
    for (int i = 0; i < g_catalog_count; i++) {
        const catalog_item_t *t = &g_catalog[i];
        const char *tid = t->tid, *state, *mount = "";
        char target[64], from[PATH_MAX];
        snprintf(target, sizeof(target), "/user/app/%s", tid);
        int mounted = mounted_from(target, from, sizeof(from));
        if (mounted) {
            char hold[PATH_MAX];
            hold_path(hold, sizeof(hold), "app", tid);
            if (top_layer(target, hold) != 1 || owned_layer_count(target, hold) < 1)
                mounted = 0;
            if (mounted && t->patch) {
                char patch_target[64], patch_hold[PATH_MAX];
                snprintf(patch_target, sizeof(patch_target), "/user/patch/%s", tid);
                hold_path(patch_hold, sizeof(patch_hold), "patch", tid);
                if (top_layer(patch_target, patch_hold) != 1 ||
                    owned_layer_count(patch_target, patch_hold) < 1)
                    mounted = 0;
            }
            if (mounted) {
                struct statfs sf;
                if (!statfs(target, &sf))
                    mount = (sf.f_flags & MNT_RDONLY) ? "ro" : "rw";
            }
        }
        managed_t *m = managed_by(tid, NULL);
        if (installed_internally(tid)) state = "internal";
        else if (m && m->running) state = "running";
        else if (mounted && db_is_registered(tid)) state = "ready";
        else if (removed(tid)) state = "removed";
        else if (!active_status(tid)) state = "inactive";
        else state = "pending";
        buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", i ? "," : "", tid);
        json_str(out, t->name);
        buf_printf(out, ",\"version\":"); json_str(out, t->ver);
        buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                   t->patch ? "true" : "false", (unsigned long long)t->size, state, mount);
        json_str(out, t->dir);
        buf_printf(out, ",\"dlc\":%d,\"update_issue\":", t->dlc);
        json_str(out, t->issue);
        buf_printf(out, "}");
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

/* Only the selected CUSA is registered; the callback merely queues a request. */
int app_add(const char *tid, char *msg, size_t cap) {
    if (redirect_locked()) {
        snprintf(msg, cap, "Restore temporary native DLC overlay first");
        return -1;
    }
    char root[32];
    if (g_restore_running || g_discover_requested || g_activate_request[0] || g_dlc_register_request[0] || g_dlc_install_request[0] ||
        !strcmp(g_progress.phase, "activating") || !strcmp(g_progress.phase, "searching") ||
        !strcmp(g_progress.phase, "registering_dlcs") || !strcmp(g_progress.phase, "dlc_register_queued") ||
        !strcmp(g_progress.phase,"installing_dlcs") || !strcmp(g_progress.phase,"dlc_queued")) {
        snprintf(msg, cap, "Busy restoring, searching, or activating another game");
        return -1;
    }
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "%s not found in the organized USB library; search first", tid);
        return -1;
    }
    if (installed_internally(tid) || title_running(tid)) {
        snprintf(msg, cap, "%s is installed internally or currently running", tid);
        return -1;
    }
    snprintf(g_activate_request, sizeof(g_activate_request), "%s", tid);
    g_progress.total = 1;
    g_progress.done = g_progress.ok = g_progress.failed = 0;
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    progress_set("queued", "Preparing selected game");
    snprintf(msg, cap, "Mount queued for %s (game + update + associated DLC metadata)", tid);
    return 0;
}

/* Never unmount unknown/foreign layers and never uninstall via Disable. */
static int write_state_atomic(const char *path, const char *tid) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (write_file(tmp, tid, strlen(tid), 0666) || rename(tmp, path)) {
        printf("[state] %s: failed to save marker: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int release_owned_layer(const char *tid, const char *kind) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/%s/%s", kind, tid);
    hold_path(hold, sizeof(hold), kind, tid);
    if (dead_layer(target)) return -1; /* no force on a stale layer */
    int count = owned_layer_count(target, hold);
    if (count < 0 || count > 16) return -1;
    for (int i = 0; i < count; ++i) {
        if (top_layer(target, hold) != 1 || unmount(target, 0)) return -1;
        int remaining = owned_layer_count(target, hold);
        if (remaining < 0 || remaining >= count - i) return -1;
    }
    return owned_layer_count(target, hold) == 0 ? 0 : -1;
}

/* Existing games can intentionally share an add-on title folder. Do not take
 * it away from a second active game when this one is disabled. */
static int other_active_title_uses_dlc(const char *tid, const char *dtid) {
    for (int i = 0; i < MAX_TITLES; ++i) {
        const managed_t *m = &g_managed[i];
        if (!m->used || !strcmp(m->tid, tid)) continue;
        for (int j = 0; j < m->ndlc_tids; ++j) {
            if (strcmp(m->dlc_tids[j], dtid)) continue;
            char target[64], hold[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", m->tid);
            hold_path(hold, sizeof(hold), "app", m->tid);
            if (top_layer(target, hold) == 1 || title_running(m->tid)) return 1;
        }
    }
    return 0;
}

/* Soft disable only the base and patch. Native DLC registration and offload
 * overlay must remain intact, including their journal and zero-byte stub.
 * Uninstalling the base via AppUnInstall cascades DLC unregistration on FW 11.40.
 * IMPORTANT: this deliberately keeps the base title registered in the PS5
 * library; the icon may still be visible but must not be launched unmounted. */
static int release_base_patch_only(const char *tid) {
    const char *kinds[]={"patch","app"};
    char target[64], hold[PATH_MAX];
    /* Preflight both targets before touching either one. Never remove a
     * foreign/hidden layer or force release after USB disconnection. */
    for (int i=0;i<2;i++) {
        snprintf(target,sizeof(target),"/user/%s/%s",kinds[i],tid);
        hold_path(hold,sizeof(hold),kinds[i],tid);
        int count=owned_layer_count(target,hold);
        int top=top_layer(target,hold);
        if (dead_layer(target) || count<0 || top<0 ||
            (count>0 && top!=1)) {
            printf("[oneclick][keep-dlcs] %s unsafe base/patch layer %s; no mounts released\n",tid,target);
            return -1;
        }
    }
    if (release_owned_layer(tid,"patch") || release_owned_layer(tid,"app")) {
        printf("[oneclick][keep-dlcs] %s base/patch release incomplete; inspect before retry\n",tid);
        return -1;
    }
    printf("[oneclick][keep-dlcs] %s base+patch unmounted; add-on mounts and DB registrations preserved\n",tid);
    return 0;
}

int app_remove(const char *tid, char *msg, size_t cap) {
    if (redirect_locked()) {
        snprintf(msg, cap, "Restore temporary native DLC overlay first");
        return -1;
    }
    if (g_restore_running || g_activate_request[0] || g_dlc_register_request[0] || g_dlc_install_request[0] || g_discover_requested ||
        !strcmp(g_progress.phase, "activating") || !strcmp(g_progress.phase, "searching") ||
        !strcmp(g_progress.phase, "registering_dlcs") || !strcmp(g_progress.phase, "dlc_register_queued")) {
        snprintf(msg, cap, "Wait until the current operation ends");
        return -1;
    }
    if (title_running(tid)) {
        snprintf(msg, cap, "Close the game before disabling it");
        return -1;
    }
    char root[32];
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "%s not found on connected USB", tid);
        return -1;
    }
    /* Persist disabled intent BEFORE releasing mounts. An I/O error must not
     * leave the game unexpectedly active again after the next reboot. */
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.disabled", tid);
    if (write_state_atomic(path, tid)) {
        snprintf(msg, cap, "Could not persist disabled status for %s", tid);
        return -1;
    }
    if (release_base_patch_only(tid)) {
        snprintf(msg,cap,"Could not safely release %s base/patch; DLCs untouched",tid);
        return -1;
    }
    /* Deliberately DO NOT call sceAppInstUtilAppUnInstall: it removes DLC
     * records. DO NOT release /user/addcont or native offload nullfs. */
    printf("[oneclick][keep-dlcs] %s DLC registry, native USB overlay and offload journal retained\n",tid);
    snprintf(path, sizeof(path), STATE_DIR "/%s.active", tid);
    unlink(path);
    managed_t *m = managed_by(tid, NULL);
    if (m) m->used = 0;
    printf("[oneclick] %s base+patch deactivated; game remains registered in PS5 library; DLCs preserved\n", tid);
    g_catalog_dirty = 1;
    snprintf(msg, cap, "%s base+patch unmounted; DLCs and PS5 registrations kept (game icon may remain)",tid);
    return 0;
}

static int active_status(const char *tid) {
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.disabled", tid);
    if (exists(path)) return 0;
    snprintf(path, sizeof(path), STATE_DIR "/%s.active", tid);
    if (exists(path)) return 1;
    /* Existing upstream 0.4.1 source markers are migrated implicitly if they
     * are STILL valid and registered: the restore itself verifies this. */
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    return exists(path);
}

static int save_active(const char *tid) {
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.active", tid);
    if (write_state_atomic(path, tid)) return -1;
    snprintf(path, sizeof(path), STATE_DIR "/%s.disabled", tid);
    unlink(path);
    return 0;
}

/* Persisted source marker is a strict path, not a discovery instruction.
 * Recover only the exact previously registered title. */
static int restore_one(const char *tid) {
    char root[32], disabled[64];
    snprintf(disabled, sizeof(disabled), STATE_DIR "/%s.disabled", tid);
    if (exists(disabled)) return -1;
    if (find_root(tid, root, sizeof(root))) {
        printf("[restore] %s: original USB absent\n", tid);
        return -1;
    }
    title_src_t t = {0};
    if (src_open(&t, root, tid)) {
        printf("[restore] %s: missing/unreadable source\n", tid);
        return -1;
    }
    /* No re-register, no metadata write, no search. */
    int rc = -1;
    if (!db_snapshot_healthy() || title_state(&t) != 0) {
        printf("[restore] %s: database/marker/files incomplete. No automatic repair\n", tid);
    } else if (mount_title(&t) || !title_layers_mounted(&t)) {
        printf("[restore] %s: mount failed or foreign layer; stopped for this title\n", tid);
    } else {
        manage(&t);
        if (save_active(tid))
            printf("[restore] %s mounted, but could not save active flag\n", tid);
        else
            rc = 0;
    }
    src_close(&t);
    return rc;
}

/* A disconnected USB must not trigger any per-title validation or a wall of failures.
 * This preflight only checks actual mounted external devices, never scans PKGs. */
static int any_external_usb(void) {
    char root[32];
    for (int i = 0; i < 10; i++) {
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (is_mountpoint(root)) return 1;
    }
    return 0;
}

/* Enumerate only persisted registrations, not PKGs in arbitrary subfolders.
 * Must be run by main AFTER the web socket exists, so the panel can show progress. */
static int restore_active_games(void) {
    if (!any_external_usb()) {
        g_progress = (progress_t){"complete", 0, 0, 0, 0, "", "Connect an external USB drive, then restart PS4Mount"};
        printf("[restore] SKIPPED: no mounted external USB; no title checks, no registration\n");
        notify("PS4Mount: external USB not connected. No restoration attempted");
        return -1; /* distinguish no USB from an empty saved library */
    }
    char tids[MAX_TITLES][10];
    int n = 0;
    DIR *d = opendir(STATE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < MAX_TITLES) {
            size_t len = strlen(e->d_name);
            if (len != 13 || strcmp(e->d_name + 9, ".src") || strncmp(e->d_name, "CUSA", 4))
                continue;
            char tid[10];
            snprintf(tid, sizeof(tid), "%.9s", e->d_name);
            int valid = 1;
            for (int k = 4; k < 9; k++) valid &= tid[k] >= '0' && tid[k] <= '9';
            if (!valid || !active_status(tid)) continue;
            snprintf(tids[n++], sizeof(tids[0]), "%s", tid);
        }
        closedir(d);
    }
    g_progress = (progress_t){"restoring", n, 0, 0, 0, "", "Checking previously active games"};
    g_restore_running = 1;
    printf("[restore] BEGIN %d previously registered title(s), no discovery/no registration\n", n);
    if (n) notify("PS4Mount: restoring %d previously active games...", n);
    for (int i = 0; i < n; i++) {
        if (g_stop) {
            printf("[restore] STOP requested; safe stop between games\n");
            break;
        }
        snprintf(g_progress.current, sizeof(g_progress.current), "%s", tids[i]);
        progress_set("restoring", "Restoring known mounts; no registration");
        int rc = restore_one(tids[i]);
        g_progress.done++;
        if (rc) g_progress.failed++;
        else g_progress.ok++;
        pump_events(1); /* service the progress panel while restoring */
    }
    g_restore_running = 0;
    g_progress.current[0] = 0;
    progress_set(g_stop ? "error" : "complete", g_stop ? "Restore stopped by user" :
                 (g_progress.failed ? "Completed with mount failures; see log" : "Previously active games restored"));
    printf("[restore] FINISHED %d/%d ok=%d failed=%d; NO RE-REGISTRATION\n", g_progress.done,
           g_progress.total, g_progress.ok, g_progress.failed);
    if (n) notify("PS4Mount: restore complete: %d/%d OK, %d failure(s)", g_progress.ok, n, g_progress.failed);
    return n;
}

static void discover_usb(void) {
    g_discover_requested = 0;
    g_scan_abort = 0;
    if (g_restore_running || g_activate_request[0] || g_dlc_register_request[0]) {
        printf("[manual][scan] busy, search ignored\n");
        return;
    }
    g_progress = (progress_t){"searching", 10, 0, 0, 0, "", "Walking USB subfolders (unknown total)"};
    printf("[manual][scan] START explicit USB discovery (PKGs may be renamed on same USB)\n");
    int moved = 0;
    for (int i = 0; i < 10; ++i) {
        if (g_scan_abort || g_stop) break;
        char root[32];
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (is_mountpoint(root)) {
            snprintf(g_progress.detail, sizeof(g_progress.detail), "Scanning %s", root);
            int count = adopt_root_pkgs(root); /* DISCOVERY ONLY, never registers */
            if (count < 0) g_progress.failed++;
            else { moved += count; g_progress.ok++; }
        }
        g_progress.done++;
        pump_events(1);
    }
    g_progress.total = g_progress.done;
    g_catalog_dirty = 1;
    snprintf(g_progress.detail, sizeof(g_progress.detail),
             "Search finished: %d folders, %d PKGs checked, %d organized",
             g_scan_folders, g_scan_examined, g_scan_organized);
    g_progress.phase = (g_scan_abort || g_stop) ? "error" : "complete";
    if (g_scan_abort || g_stop)
        snprintf(g_progress.detail, sizeof(g_progress.detail),
                 "USB disconnected or search interrupted: %d folders, %d PKGs checked", g_scan_folders, g_scan_examined);
    printf("[manual][scan] %s moved=%d (same USB), NO registration\n",
           g_scan_abort ? "ABORT" : "DONE", moved);
    if (g_scan_abort || g_stop)
        notify("PS4Mount: USB search interrupted; %d PKG(s) organized before interruption", moved);
    else
        notify("PS4Mount: USB scan complete. %d PKG(s) organized; select a game to activate", moved);
}

/* A PS5 title already registered by the system does NOT need another
 * installer call merely because a former PS4Mount marker is missing/stale.
 * An explicit Mount may recreate only this title's USB-derived files/links.
 * Never perform a global registration, a DB write or force an occupied mount. */
static int hold_link_is(const char *kind, const char *tid, const char *name, const char *expected) {
    char link[PATH_MAX], actual[PATH_MAX];
    hold_path(link, sizeof(link), kind, tid);
    size_t n = strlen(link);
    snprintf(link + n, sizeof(link) - n, "/%s", name);
    ssize_t r = readlink(link, actual, sizeof(actual) - 1);
    if (r <= 0) return 0;
    actual[r] = 0;
    return !strcmp(actual, expected);
}

/* Reports database evidence only.  Mounting ac.pkg is not equivalent to a
 * registered entitlement; do not call AppInstallAll or install packages to
 * internal storage as a hidden side effect of the Mount button. */
/* One-click native registration -> USB offload helpers (implemented below). */
static int offload_one(const dlc_src_t *dlc, const char *tid);
static int offload_wait_native_ready(const dlc_src_t *dlc, const char *tid, int status_unavailable);
static int offload_resume_title(const char *tid);
static void offload_resume_all(void);

static void report_dlc_registration(const title_src_t *t) {
    if (!t->ndlc) return;
    const char *ids[MAX_DLC];
    int states[MAX_DLC], registered = 0, missing = 0, unknown = 0;
    for (int i = 0; i < t->ndlc; ++i) ids[i] = t->dlc[i].pkg.content_id;
    db_dlc_registration(ids, t->ndlc, states);
    for (int i = 0; i < t->ndlc; ++i) {
        if (states[i] == 1) registered++;
        else if (states[i] == 0) {
            missing++;
            printf("[manual][dlc-db] %s: NO DATABASE RECORD FOUND for %s\n", t->app.title_id, ids[i]);
        } else unknown++;
    }
    printf("[manual][dlc-db] %s: sources=%d database_registered=%d missing=%d unknown=%d; mounted files alone do not prove DLC entitlement\n",
           t->app.title_id, t->ndlc, registered, missing, unknown);
    if (missing)
        printf("[manual][dlc-db] %s: DLC registration NOT confirmed; targeted base registration does not prove add-on registration\n", t->app.title_id);
    if (unknown)
        printf("[manual][dlc-db] %s: addcont.db unreadable or schema not recognized; no registration claim\n", t->app.title_id);
}

/* Targeted, NO-COPY revalidation of a prepared USB bundle.
 * We deliberately use the *existing* title registration function with its
 * documented /user/app/ root, NOT InstallAll and NOT InstallByPackage.
 * Firmware 11.40 has not proved that it also promotes addcont records.
 * All results below are based on read-only addcont.db evidence. */
static int usb_dlc_revalidate(title_src_t *t, const char *why) {
    if (!t->ndlc) return 0;
    const char *tid = t->app.title_id;
    const char *ids[MAX_DLC];
    int before[MAX_DLC], after[MAX_DLC];
    int known = 0, added = 0, missing = 0, unknown = 0;
    for (int i = 0; i < t->ndlc; i++) ids[i] = t->dlc[i].pkg.content_id;
    db_dlc_registration(ids, t->ndlc, before);
    for (int i = 0; i < t->ndlc; i++) if (before[i] == 1) known++;
    printf("[dlc-usb] %s %s: staged=%d db_before=%d; probing title-only revalidation (UNPROVEN for DLCs)\n",
           tid, why, t->ndlc, known);
    /* An installed native DLC can mask this mount. Make this visible in logs;
     * never delete or override the internal add-on to fake a USB-only result. */
    for (int i = 0; i < t->ndlc_tids; i++) {
        if (addcont_installed(t->dlc_tids[i]))
            printf("[dlc-usb] %s: /user/addcont/%s has internal files; NOT USB-only for this folder\n",
                   tid, t->dlc_tids[i]);
    }
    if (!g_appinst_ready || !db_snapshot_healthy() || !db_is_registered(tid) ||
        title_running(tid) || !title_layers_mounted(t)) {
        printf("[dlc-usb] %s: revalidation not attempted (service/DB/mount/running state)\n", tid);
        return -1;
    }
    app_install_title_dir_fn single = one_title_install_api();
    if (!single) {
        printf("[dlc-usb] %s: AppInstallTitleDir unavailable; NO global fallback\n", tid);
        return -1;
    }
    progress_set(g_progress.phase, "Revalidando registro local; DLCs continuam no USB");
    int rc = single(tid, "/user/app/", NULL);
    printf("[dlc-usb] %s AppInstallTitleDir(/user/app/) rc=0x%08x (not an addcont-specific API)\n",
           tid, (unsigned)rc);
    /* 0x80990002 is often an already-known title. Neither code proves DLC registration. */
    if (rc && (uint32_t)rc != 0x80990002u) return -1;
    wait_db_quiet(2, 12);
    db_dlc_registration(ids, t->ndlc, after);
    for (int i = 0; i < t->ndlc; i++) {
        if (after[i] == 1) {
            if (before[i] != 1) added++;
        } else if (after[i] == 0) {
            missing++;
            printf("[dlc-usb] %s STILL_MISSING %s\n", tid, ids[i]);
        } else {
            unknown++;
            printf("[dlc-usb] %s UNKNOWN %s\n", tid, ids[i]);
        }
    }
    printf("[dlc-usb] %s AFTER: total=%d newly_registered=%d registered=%d missing=%d unknown=%d\n",
           tid, t->ndlc, added, t->ndlc - missing - unknown, missing, unknown);
    if (missing || unknown)
        printf("[dlc-usb] %s: targeted title refresh DID NOT prove add-on registration; no PKGs copied\n", tid);
    else
        printf("[dlc-usb] %s: all DLC records present (might predate this action); no PKGs copied by PS4Mount\n", tid);
    return (missing || unknown) ? -1 : 0;
}

static int mount_registered_title(title_src_t *t) {
    const char *tid = t->app.title_id;
    char root[32];
    if (find_root(tid, root, sizeof(root)) || strncmp(t->dir, root, strlen(root)) ||
        t->dir[strlen(root)] != '/') {
        printf("[manual][mount] %s: disconnected or changed USB source\n", tid);
        return -1;
    }
    if (target_mounts_safe(t)) {
        printf("[manual][mount] %s: occupied/unreadable mount target; cannot replace another layer\n", tid);
        return -1;
    }
    char pdb[PATH_MAX];
    snprintf(pdb, sizeof(pdb), "/user/playgo/app/%s/00/d0.pdb", t->app.content_id);
    int refresh = !marker_matches(tid, t->marker) || !exists(pdb) ||
                  !hold_link_is("app", tid, "app.pkg", t->app_path) ||
                  (t->has_patch && !hold_link_is("patch", tid, "patch.pkg", t->patch_path)) ||
                  !dlc_ready(t);
    if (refresh) {
        printf("[manual][mount] %s: existing DB registration, rebuilding USB metadata (no installer)\n", tid);
        progress_set("activating", "Refreshing metadata and USB links of registered game");
        if (place_files(t)) {
            printf("[manual][mount] %s: preparing metadata FAILED\n", tid);
            return -1;
        }
    } else {
        printf("[manual][mount] %s: existing DB registration, prepared files reusable\n", tid);
    }
    progress_set("activating", "Montando jogo base, atualizacao e DLCs");
    if (mount_title(t) || !title_layers_mounted(t)) {
        printf("[manual][mount] %s: incomplete/failed filesystem mount\n", tid);
        return -1;
    }
    if (refresh && register_finish(t)) {
        printf("[manual][mount] %s: saving refreshed title marker FAILED\n", tid);
        return -1;
    }
    if (save_active(tid)) {
        printf("[manual][mount] %s: saving active state FAILED\n", tid);
        return -1;
    }
    manage(t);
    printf("[manual][mount] %s: mounted using existing PS5 registration, no ownership marker required\n", tid);
    return 0;
}

static void activate_queued(void) {
    if (!g_activate_request[0]) return;
    char tid[10], root[32];
    snprintf(tid, sizeof(tid), "%s", g_activate_request);
    g_activate_request[0] = 0;
    g_progress = (progress_t){"activating", 1, 0, 0, 0, "", "Checking selected title"};
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    printf("[manual][activate] %s START (single-title, no AppInstallAll)\n", tid);
    /* Resolve targeted call before any staging. */
    app_install_title_dir_fn single = one_title_install_api();
    if (find_root(tid, root, sizeof(root))) goto failed;
    title_src_t t = {0};
    if (src_open(&t, root, tid)) goto failed;
    printf("[manual][dlc] %s discovered=%d valid PKG(s), folders=%d (removed markers ignored)\n",
           tid, t.ndlc, t.ndlc_tids);
    /* An explicit click may re-enable a title removed in an earlier upstream release. */
    char removed_path[64];
    snprintf(removed_path, sizeof(removed_path), STATE_DIR "/%s.removed", tid);
    if (removed(tid)) {
        printf("[manual][activate] %s explicitly selected; clearing old removed flag\n", tid);
        unlink(removed_path);
    }
    int db_ok = db_snapshot_healthy();
    int registered = db_ok && db_is_registered(tid);
    int rc = -1;
    if (!db_ok) printf("[manual][activate] app.db unreadable/uncertain; NO metadata staged\n");
    char attempt[64];
    snprintf(attempt, sizeof(attempt), STATE_DIR "/%s.attempt", tid);
    if (registered) {
        /* Any individually selected, already-registered USB game can be mounted,
         * including titles from 0.4.1 or previously unmounted in this panel. */
        rc = mount_registered_title(&t);
    } else if (db_ok && !registered) {
        if (!single) {
            printf("[manual][activate] targeted registration API unavailable, no staging\n");
        } else if (exists(attempt)) {
            printf("[manual][activate] %s prior interrupted/failed attempt; inspect before retry\n", tid);
        } else if (!is_mountpoint(root)) {
            printf("[manual][activate] %s USB source disappeared; NOTHING staged\n", tid);
        } else if (target_mounts_safe(&t)) {
            printf("[manual][activate] %s targets not safe; NOTHING staged\n", tid);
        } else if (write_file(attempt, tid, strlen(tid), 0666)) {
            printf("[manual][activate] %s cannot persist attempt guard; nothing staged\n", tid);
        } else {
            progress_set("activating", "Preparing game, update, and DLC files");
            if (!register_prepare(&t) && title_layers_mounted(&t)) {
                progress_set("activating", "Requesting targeted registration");
                int api_rc = single(tid, "/user/app/", NULL);
                printf("[manual][activate] %s AppInstallTitleDir rc=0x%08x\n", tid, api_rc);
                if (api_rc == 0 || (uint32_t)api_rc == 0x80990002u) {
                    /* Wait for ShellCore, do not write app.db ourselves. */
                    for (int i = 0; i < 12 && !db_is_registered(tid); i++) pump_events(400);
                    wait_db_quiet(3, 30);
                    if (db_snapshot_healthy() && !register_finish(&t) && !save_active(tid)) {
                        unlink(attempt);
                        rc = 0;
                    }
                }
            }
        }
    } else {
        printf("[manual][activate] %s: database unreadable; mount postponed\n", tid);
    }
    if (!rc) {
        printf("[manual][activate] %s activated: game+update+DLC mounts checked; %d DLC source(s) staged\n",
               tid, t.ndlc);
        report_dlc_registration(&t);
    }
    int found_dlcs = t.ndlc;
    src_close(&t);
    if (rc) goto failed;
    g_progress.ok = 1;
    g_progress.done = 1;
    g_catalog_dirty = 1;
    /* Same click: after base registration, install missing DLCs then offload.
     * A second main-loop iteration handles this without blocking the panel. */
    if (found_dlcs) {
        snprintf(g_dlc_install_request, sizeof(g_dlc_install_request), "%s", tid);
        g_progress = (progress_t){"dlc_queued", found_dlcs, 0, 0, 0, "", "Montagem concluida; instalando e mapeando DLCs"};
        snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
        printf("[oneclick] %s stage 1/2 base mounted; stage 2/2 queued DLC native register + USB offload (%d)\n",tid,found_dlcs);
    } else {
        progress_set("complete", "Jogo e atualizacao montados (sem DLCs)");
        notify("PS4Mount: %s montado sem DLCs", tid);
    }
    return;
failed:
    g_progress.failed = 1;
    g_progress.done = 1;
    progress_set("error", "Mount failed or title state uncertain. Inspect log before trying again");
    printf("[manual][activate] %s FAILED; no automatic retry\n", tid);
    notify("PS4Mount: %s mount FAILED. Check the log", tid);
}


/* Read-only, CUSA-scoped inspection of the PS5 addcont.db. This does not
 * install/revalidate a title, unmount anything, or write to the system DBs.
 * Useful to compare native-installed Resident Evil 7 add-ons with absent
 * God of War Ragnarök add-on rows. */
int app_diagnose_db(const char *tid, char *msg, size_t cap) {
    if (g_discover_requested || g_restore_running || g_activate_request[0] ||
        g_dlc_register_request[0] || g_dlc_install_request[0] || !strcmp(g_progress.phase, "registering_dlcs") ||
        !strcmp(g_progress.phase, "activating") || !strcmp(g_progress.phase, "searching")) {
        snprintf(msg, cap, "An operation is running. Retry when idle");
        return -1;
    }
    printf("[db-inspect] %s: app.db title registered=%d (read-only snapshot)\n",
           tid, db_is_registered(tid));
    int rc = db_log_addcont_title(tid);
    if (rc == -1) {
        snprintf(msg, cap, "DB read failed; inspect log for %s", tid);
        return -1;
    }
    snprintf(msg, cap, "Diagnostic logged for %s: %d matching DB row(s)", tid, rc);
    return 0;
}

/* Experimental registration matrix: each button tries EXACTLY one targeted
 * AppInstallTitleDir variant for ONE DLC chosen from the actual USB source.
 * It does not skip a DLC due to preexisting DB state or .removed markers.
 * A successful API return is not considered proof; the read-only addcont.db
 * snapshot before/after is the only evidence for NEW_DB_RECORD.
 * IMPORTANT: these unverified call shapes can still have OS side effects. */
/* Independent Retry button. The same targeted no-copy revalidation is also
 * attempted on Mount. We NEVER uninstall and NEVER run a package installer. */
int app_register_dlcs(const char *tid, char *msg, size_t cap) {
    if (redirect_locked()) {
        snprintf(msg, cap, "Restore temporary native DLC overlay first");
        return -1;
    }
    char root[32];
    if (g_dlc_register_request[0] || g_dlc_install_request[0] || g_activate_request[0] || g_discover_requested ||
        g_restore_running || !strcmp(g_progress.phase, "registering_dlcs") ||
        !strcmp(g_progress.phase, "activating") || !strcmp(g_progress.phase, "searching") ||
        !strcmp(g_progress.phase, "dlc_register_queued")) {
        snprintf(msg, cap, "Another operation is running");
        return -1;
    }
    if (!g_appinst_ready || !one_title_install_api()) {
        snprintf(msg, cap, "Targeted AppInstallTitleDir unavailable; no global fallback");
        return -1;
    }
    if (find_root(tid, root, sizeof(root)) || !is_mountpoint(root) ||
        title_running(tid) || installed_internally(tid) ||
        !db_snapshot_healthy() || !db_is_registered(tid)) {
        snprintf(msg, cap, "Game must be mounted, on USB, registered and closed");
        return -1;
    }
    snprintf(g_dlc_register_request, sizeof(g_dlc_register_request), "%s", tid);
    g_progress = (progress_t){"dlc_register_queued", 0, 0, 0, 0, "", "Aguardando revalidacao dos DLCs no USB"};
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    printf("[dlc-usb] %s queued, no native PKG installer, no AppInstallAll\n", tid);
    snprintf(msg, cap, "USB DLC registration probe queued for %s", tid);
    return 0;
}

static void register_dlcs_queued(void) {
    if (!g_dlc_register_request[0]) return;
    char tid[10], root[32];
    snprintf(tid, sizeof(tid), "%s", g_dlc_register_request);
    g_dlc_register_request[0] = 0;
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    progress_set("registering_dlcs", "Validando montagem e metadados no USB");
    title_src_t t = {0};
    int opened = 0, rc = -1;
    if (find_root(tid, root, sizeof(root)) || !is_mountpoint(root) ||
        !db_snapshot_healthy() || !db_is_registered(tid) || title_running(tid) ||
        installed_internally(tid)) goto out;
    if (src_open(&t, root, tid)) goto out;
    opened = 1;
    if (!t.ndlc) {
        printf("[dlc-usb] %s: no valid DLC PKGs on USB\n", tid);
        goto out;
    }
    if (!title_layers_mounted(&t) || target_mounts_safe(&t)) {
        printf("[dlc-usb] %s: game/patch/addcont USB bundle not safely mounted\n", tid);
        goto out;
    }
    g_progress.total = t.ndlc;
    /* Recreate only metadata and symlinks, never install PKG contents. */
    if (place_files(&t) || mount_title(&t) || !title_layers_mounted(&t)) {
        printf("[dlc-usb] %s: failed to refresh USB metadata\n", tid);
        goto out;
    }
    rc = usb_dlc_revalidate(&t, "BUTTON");
    report_dlc_registration(&t);
    g_progress.done = t.ndlc;
    if (!rc) g_progress.ok = t.ndlc;
    else g_progress.failed = t.ndlc;
out:
    if (opened) src_close(&t);
    g_catalog_dirty = 1;
    progress_set(rc ? "dlc_error" : "dlc_complete",
                 rc ? "Registro dos DLCs NAO confirmado; veja o log" : "Registros de DLC presentes no banco");
    printf("[dlc-usb] %s explicit probe %s\n", tid, rc ? "NOT_CONFIRMED" : "DB_CONFIRMED");
}

/* Explicit per-title request. Never invoked by Mount, USB scan or restoration.
 * Installation is a native PKG operation; data MAY be copied to internal storage.
 * Unlike external mounts, internal add-on installations are not removed on Unmount. */
int app_install_dlcs(const char *tid, char *msg, size_t cap) {
    char root[32];
    if (g_dlc_install_request[0] || g_dlc_register_request[0] || g_activate_request[0] || g_discover_requested ||
        g_restore_running || !strcmp(g_progress.phase, "installing_dlcs") ||
        !strcmp(g_progress.phase, "activating") || !strcmp(g_progress.phase, "searching") ||
        !strcmp(g_progress.phase, "installing_dlcs") || !strcmp(g_progress.phase, "dlc_queued")) {
        snprintf(msg, cap, "Another operation is running");
        return -1;
    }
    if (redirect_locked()) {
        snprintf(msg, cap, "Restore the RE7 USB overlay before native DLC installation");
        return -1;
    }
    if (!g_appinst_ready) {
        snprintf(msg, cap, "PS5 AppInstUtil initialization failed; see startup log");
        return -1;
    }
    if (find_root(tid, root, sizeof(root)) || !is_mountpoint(root)) {
        snprintf(msg, cap, "%s: USB storage not connected", tid);
        return -1;
    }
    if (title_running(tid) || installed_internally(tid)) {
        snprintf(msg, cap, "Close the game and use an external USB game");
        return -1;
    }
    if (!db_snapshot_healthy() || !db_is_registered(tid)) {
        snprintf(msg, cap, "Base game must already be registered on PS5");
        return -1;
    }
    snprintf(g_dlc_install_request, sizeof(g_dlc_install_request), "%s", tid);
    g_dlc_percent = -1;
    g_dlc_content[0] = 0;
    g_progress = (progress_t){"dlc_queued", 0, 0, 0, 0, "", "Aguardando instalacao individual dos DLCs"};
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    printf("[dlc-install] %s queued (individual PKGs, never AppInstallAll)\n", tid);
    snprintf(msg, cap, "Individual DLC installation queued for %s", tid);
    return 0;
}

/* One selected DLC; the normal full-install button is still available for other games. */
int app_install_dlc_one(const char *tid, const char *cid, char *msg, size_t cap) {
    if (strcmp(tid, "CUSA34384") || strlen(cid) != 36 ||
        strncmp(cid + 7, tid, 9) || cid[16] != '_' || cid[17] != '0' || cid[18] != '0' || cid[19] != '-') {
        snprintf(msg, cap, "Piloto individual apenas para CUSA34384 e Content ID valido");
        return -1;
    }
    if (app_install_dlcs(tid, msg, cap)) return -1;
    snprintf(g_dlc_install_only_cid, sizeof(g_dlc_install_only_cid), "%s", cid);
    printf("[dlc-pilot] SINGLE NATIVE INSTALL queued %s (copies one PKG to SSD)\n", cid);
    snprintf(msg, cap, "Instalacao nativa de 1 DLC enfileirada: %s", cid);
    return 0;
}

/* Selective layer release: an external nullfs add-on folder must not mask a
 * native installer destination. Do not touch base/patch or non-owned mounts. */
static int dlc_installer_release_external_layer(const char *dtid) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/addcont/%s", dtid);
    hold_path(hold, sizeof(hold), "addcont", dtid);
    int n = owned_layer_count(target, hold);
    int top = top_layer(target, hold);
    if (n < 0 || top < 0 || dead_layer(target)) return -1;
    if (n == 0) return top == 0 ? 0 : -1;
    if (top != 1) return -1;
    if (release_owned_layer(dtid, "addcont")) return -1;
    printf("[dlc-install] released PS4Mount add-on overlay %s for native installer\n", dtid);
    return 0;
}

static int dlc_status_terminal(const char *s) {
    return !strcmp(s, "playable") || !strcmp(s, "completed") ||
           !strcmp(s, "installed") || !strcmp(s, "done");
}

/* 0=terminal success, 1=status service cannot find this task (recheck on-disk),
 * -1=explicit failure, -2=other uncertain task. Database + native file must
 * still be verified by the caller before any offload. */
static int dlc_native_install_one(const dlc_src_t *dlc) {
    struct stat st;
    if (stat(dlc->path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        printf("[dlc-install] %s missing source: %s\n", dlc->pkg.content_id, dlc->path);
        return -1;
    }
    ps5_pkg_meta_t meta = {
        .uri = dlc->path, .ex_uri = "", .playgo_scenario_id = "",
        .content_id = "", .content_name = dlc->pkg.content_id, .icon_url = ""
    };
    ps5_pkg_info_t info = {0};
    ps5_playgo_info_t *playgo = calloc(1, sizeof(*playgo));
    if (!playgo) return -1;
    printf("[dlc-install] START %s from %s (%llu bytes)\n", dlc->pkg.content_id,
           dlc->path, (unsigned long long)st.st_size);
    int rc = sceAppInstUtilInstallByPackage(&meta, &info, playgo);
    free(playgo);
    printf("[dlc-install] request %s rc=0x%08x returned_id=%.36s\n",
           dlc->pkg.content_id, (unsigned)rc, info.content_id);
    if (rc) return -1;
    /* Use the actual content_id returned by AppInst; the requested ID is the
     * fallback if an implementation leaves that field blank. */
    info.content_id[sizeof(info.content_id)-1] = 0;
    const char *cid = info.content_id[0] ? info.content_id : dlc->pkg.content_id;
    if (strcmp(cid, dlc->pkg.content_id)) {
        printf("[dlc-install] Content ID mismatch %s != %s (aborting)\n",
               cid, dlc->pkg.content_id);
        return -2; /* accepted task, but mismatched ID: do not start another */
    }
    int untracked_retries = 0;
    for (int second = 0; second < 1800; ++second) {
        ps5_install_status_t status = {0};
        rc = sceAppInstUtilGetInstallStatus(cid, &status);
        status.status[sizeof(status.status)-1] = 0;
        if (rc) {
            if ((unsigned)rc == 0x80a30003U) {
                /* On FW 11.40 the native installer can report this even
                 * though the same Content ID appears in addcont.db later.
                 * Retry briefly; a missing task is NOT a completed install. */
                if (++untracked_retries < 5) {
                    printf("[dlc-install] %s status untracked 0x80a30003 (%d/5), retrying\n",
                           cid, untracked_retries);
                    pump_events(1000);
                    continue;
                }
                printf("[dlc-install] %s status unavailable 0x80a30003; verifying native file and DB before offload\n", cid);
                return 1;
            }
            printf("[dlc-install] status query %s failed rc=0x%08x; task outcome unknown\n", cid, (unsigned)rc);
            return -2;
        }
        untracked_retries = 0;
        if (status.total_size > 0) {
            unsigned long long pct = status.downloaded_size * 100ULL / status.total_size;
            g_dlc_percent = (int)(pct > 100 ? 100 : pct);
        } else if (status.local_copy_percent >= 0 && status.local_copy_percent <= 100 &&
                   status.local_copy_percent != 0) {
            g_dlc_percent = status.local_copy_percent;
        }
        if (second % 5 == 0 || dlc_status_terminal(status.status) ||
            !strcmp(status.status, "error") || !strcmp(status.status, "none"))
            printf("[dlc-install] %s status=%s transfer=%llu/%llu progress=%d%% copy=%d promote=%u error=0x%08x\n",
                   cid, status.status, (unsigned long long)status.downloaded_size,
                   (unsigned long long)status.total_size, g_dlc_percent,
                   status.local_copy_percent, status.promote_progress,
                   (unsigned)status.error_info.error_code);
        if (dlc_status_terminal(status.status)) return 0;
        if (!strcmp(status.status, "error")) return -1;
        /* A newly accepted task can briefly report none before its status
         * becomes visible. Past that window, consider its fate uncertain. */
        if (!strcmp(status.status, "none") && second >= 4) return -2;
        pump_events(1000);
    }
    printf("[dlc-install] %s still processing after 30 minutes; no new package started\n", cid);
    return -2;
}

static void install_dlcs_queued(void) {
    if (!g_dlc_install_request[0]) return;
    char tid[10], root[32];
    snprintf(tid, sizeof(tid), "%s", g_dlc_install_request);
    g_dlc_install_request[0] = 0;
    char only_cid[37];
    snprintf(only_cid, sizeof(only_cid), "%s", g_dlc_install_only_cid);
    g_dlc_install_only_cid[0] = 0;
    snprintf(g_progress.current, sizeof(g_progress.current), "%s", tid);
    progress_set("installing_dlcs", "Verificando registros dos DLCs");
    title_src_t t = {0};
    int opened = 0;
    int fatal = 0;
    int native_task_unknown = 0;
    if (find_root(tid, root, sizeof(root)) || !is_mountpoint(root) ||
        !db_snapshot_healthy() || !db_is_registered(tid) || title_running(tid)) {
        printf("[dlc-install] %s invalid base registration, storage or running state\n", tid);
        goto failed;
    }
    if (src_open(&t, root, tid)) goto failed;
    opened = 1;
    printf("[dlc-install] %s discovered=%d PKG(s) for explicit install (removed flags ignored)\n", tid, t.ndlc);
    if (!t.ndlc) {
        printf("[dlc-install] %s no DLC PKGs in organized USB folder\n", tid);
        goto failed;
    }
    const char *ids[MAX_DLC];
    int states[MAX_DLC];
    for (int i = 0; i < t.ndlc; ++i) ids[i] = t.dlc[i].pkg.content_id;
    db_dlc_registration(ids, t.ndlc, states);
    /* One click processes each DLC. Already-registered DLCs are not reinstalled:
     * they may already have a zero-byte native stub from previous offloading. */
    int todo = only_cid[0] ? 1 : t.ndlc;
    if (only_cid[0]) {
        int found = 0;
        for (int i = 0; i < t.ndlc; ++i)
            if (!strcmp(t.dlc[i].pkg.content_id, only_cid)) found = 1;
        if (!found) {
            printf("[dlc-pilot] UNKNOWN USB DLC %s: refusing installation\n", only_cid);
            fatal = 1;
            goto done;
        }
    }
    int already = 0;
    for (int i = 0; i < t.ndlc; i++) if (states[i] == 1) already++;
    g_progress.total = todo;
    g_progress.done = g_progress.failed = g_progress.ok = 0;
    printf("[dlc-install] %s found=%d already_in_db=%d attempting_all=%d\n",
           tid, t.ndlc, already, todo);
    if (!todo) goto done;
    /* Keep existing add-on mounts intact when every selected DLC is already
     * registered: remounting the base must not disturb a valid USB offload.
     * Only clear external add-on layers when a NATIVE installer is needed. */
    int needs_native_installer=0;
    for(int i=0;i<t.ndlc;i++) {
        if ((!only_cid[0] || !strcmp(t.dlc[i].pkg.content_id,only_cid)) &&
            states[i]!=1) needs_native_installer=1;
    }
    if (needs_native_installer) {
        for (int i=0; i<t.ndlc_tids; ++i) {
            if (other_active_title_uses_dlc(tid,t.dlc_tids[i]) ||
                dlc_installer_release_external_layer(t.dlc_tids[i])) {
                printf("[dlc-install] %s could not safely clear add-on overlay %s\n",tid,t.dlc_tids[i]);
                fatal=1;
                goto done;
            }
        }
    } else {
        printf("[oneclick][keep-dlcs] %s all selected DLCs registered; no installer and no add-on unmount\n",tid);
    }
    for (int i = 0; i < t.ndlc; ++i) {
        if (only_cid[0] && strcmp(t.dlc[i].pkg.content_id, only_cid)) continue;
        if (!is_mountpoint(root) || title_running(tid)) { fatal = 1; break; }
        snprintf(g_dlc_content, sizeof(g_dlc_content), "%s", t.dlc[i].pkg.content_id);
        g_dlc_percent = -1;
        snprintf(g_progress.detail, sizeof(g_progress.detail), "Instalando DLC %d/%d: %.36s",
                 g_progress.done + 1, todo, g_dlc_content);
        const char *id = t.dlc[i].pkg.content_id;
        int state = -1;
        db_dlc_registration(&id, 1, &state);
        int rc = 0;
        int installer_status_unavailable = 0;
        if (state == 1) {
            printf("[oneclick] %s already registered, skipping installer; checking USB offload\n", id);
        } else if (state == 0) {
            rc = dlc_native_install_one(&t.dlc[i]);
            installer_status_unavailable = (rc == 1);
            if (installer_status_unavailable) rc = 0; /* NOT success; conditional verification below */
            if (!rc) {
                wait_db_quiet(2,15);
                db_dlc_registration(&id,1,&state);
                if (state != 1) {
                    printf("[dlc-install] %s DB not yet registered: state=%d; status_unknown=%d\n",
                           id, state, installer_status_unavailable);
                    /* For 0x80a30003 only, allow the readiness monitor to
                     * wait for a DB row and full native file. A terminal API
                     * success without a DB row also needs the same checks. */
                }
            }
        } else rc = -1; /* never delete a file if the DB is unreadable */
        if (!rc) {
            progress_set("installing_dlcs", "Verificando arquivo nativo e estabilidade da instalacao");
            if (offload_wait_native_ready(&t.dlc[i], tid, installer_status_unavailable)) {
                printf("[dlc-install] %s offload deferred: native installation/file state not confirmed\n", id);
                rc = -2; /* possible active native task: avoid unsafe remount */
            } else {
                progress_set("installing_dlcs", "Arquivo verificado; redirecionando e liberando SSD");
                rc = offload_one(&t.dlc[i],tid);
            }
        }
        if (rc) g_progress.failed++;
        else g_progress.ok++;
        if (rc == -2) {
            fatal = 1;
            native_task_unknown = 1; /* may still be running: do not remount add-on paths */
        }
        g_progress.done++;
        printf("[dlc-install] %s result=%s (%d/%d)\n", t.dlc[i].pkg.content_id,
               rc ? "FAILED_OR_NOT_REGISTERED" : "REGISTERED", g_progress.done, todo);
        if (fatal) break;
    }
done:
    /* Native add-ons take precedence. Rebuild our external mounts only for
     * folders still without internal installed contents. */
    if (opened && !native_task_unknown && mount_title(&t)) {
        printf("[dlc-install] %s could not restore external bundle after installer\n", tid);
        fatal = 1;
    }
    if (native_task_unknown)
        printf("[dlc-install] %s native task status UNKNOWN: external add-on overlay not restored to avoid hiding an ongoing install\n", tid);
    if (opened) {
        report_dlc_registration(&t);
        src_close(&t);
    }
    g_dlc_percent = -1;
    g_dlc_content[0] = 0;
    g_catalog_dirty = 1;
    if (fatal || g_progress.failed) {
        progress_set("dlc_error", "Instalacao incompleta; consulte o log de DLCs");
        printf("[dlc-install] %s finished with errors: success=%d failed=%d fatal=%d\n",
               tid, g_progress.ok, g_progress.failed, fatal);
    } else {
        progress_set("dlc_complete", "DLCs registrados, apontados ao USB e SSD liberado (consulte log)");
        printf("[dlc-install] %s finished: %d/%d registered\n", tid, g_progress.ok, todo);
    }
    return;
failed:
    if (opened) src_close(&t);
    g_dlc_percent = -1;
    g_dlc_content[0] = 0;
    g_progress.failed++;
    progress_set("dlc_error", "Nao foi possivel iniciar instalacao de DLCs");
    printf("[dlc-install] %s unable to start\n", tid);
}



/* Reversible, SEQUENTIAL experiment for 17 natively-installed RE7 DLCs.
 * Each DLC is compared and immediately mounted before the next is processed.
 * Failure rolls back only this test's successfully mounted overlays.
 * No database writes, native installation, AppInstallAll or deletion. */
#define REDIRECT_TID "CUSA03842"
#define REDIRECT_COUNT 17
#define REDIRECT_NATIVE_ROOT "/mnt/ext1/user/addcont/" REDIRECT_TID
#define REDIRECT_SOURCE_ROOT HOLD_DIR "/redirect-native/" REDIRECT_TID

static const char *g_redirect_labels[REDIRECT_COUNT] = {
    "BH70000000DLC001", "BH70000000DLC002", "BH70000000DLC003",
    "BH70000000DLC004", "BH70000000DLC005", "BH70000000DLC006",
    "BH7000COINDLC001", "BH7000COINDLC002", "BH7000COINDLC003",
    "BH7000COINDLC004", "BH7000COINDLC005",
    "BH7EPISODEDLC011", "BH7EPISODEDLC012", "BH7EPISODEDLC013",
    "BH7EPISODEDLC014", "BH7EPISODEDLC020", "BH7EPISODEDLC030"
};
/* 0=not run, 1=validating, 2=mounting, 3=success, 4=failed, 5=restored. */
static int g_redirect_job_active;
static int g_redirect_stage;
static int g_redirect_done;
static int g_redirect_current;

static void redirect_paths(int i, char *native, size_t nn, char *source, size_t sn) {
    snprintf(native, nn, REDIRECT_NATIVE_ROOT "/%s", g_redirect_labels[i]);
    snprintf(source, sn, REDIRECT_SOURCE_ROOT "/%s", g_redirect_labels[i]);
}

static int redirect_mounted(void) {
    char native[PATH_MAX], source[PATH_MAX];
    for (int i = 0; i < REDIRECT_COUNT; i++) {
        redirect_paths(i, native, sizeof(native), source, sizeof(source));
        if (top_layer(native, source) == 1 && owned_layer_count(native, source) == 1)
            return 1;
    }
    return 0;
}

static int redirect_locked(void) {
    return __atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE) || redirect_mounted() || pilot_locked();
}

static int redirect_usb_missing(void) {
    char native[PATH_MAX], source[PATH_MAX], pkg[PATH_MAX];
    for (int i = 0; i < REDIRECT_COUNT; i++) {
        redirect_paths(i, native, sizeof(native), source, sizeof(source));
        if (top_layer(native, source) != 1) continue;
        snprintf(pkg, sizeof(pkg), "%s/ac.pkg", native);
        if (!exists(pkg)) return 1;
    }
    return 0;
}

/* Worker-only running check with its own mount-table allocation. The regular
 * title_running() uses a shared cached array intended for the main thread. */
static int redirect_title_running(void) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0) return 1; /* fail closed if mount state cannot be read */
    struct statfs *list = calloc((size_t)n + 16, sizeof(*list));
    if (!list) return 1;
    n = getfsstat(list, (long)((size_t)(n + 16) * sizeof(*list)), MNT_NOWAIT);
    if (n < 0) { free(list); return 1; }
    int found = 0;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(list[i].f_mntonname, "/mnt/sandbox/pfsmnt/" REDIRECT_TID "-app0")) {
            found = 1;
            break;
        }
    }
    free(list);
    return found;
}

static int redirect_busy(void) {
    return __atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE) ||
           g_discover_requested || g_restore_running || g_activate_request[0] ||
           g_dlc_register_request[0] || g_dlc_install_request[0] || g_deleting ||
           !strcmp(g_progress.phase, "activating") ||
           !strcmp(g_progress.phase, "installing_dlcs") ||
           !strcmp(g_progress.phase, "registering_dlcs") ||
           !strcmp(g_progress.phase, "searching") ||
           !strcmp(g_progress.phase, "queued") ||
           !strcmp(g_progress.phase, "dlc_queued") ||
           !strcmp(g_progress.phase, "dlc_register_queued");
}

static void redirect_status(int stage, int done) {
    __atomic_store_n(&g_redirect_done, done, __ATOMIC_RELEASE);
    __atomic_store_n(&g_redirect_stage, stage, __ATOMIC_RELEASE);
}

/* Exact byte comparison, including PKGs >4GiB. Buffers live on heap because
 * allocating two 64KiB arrays on a PS5 worker thread can exhaust its stack. */
static int redirect_same_file_bytes(const char *native, const char *usb) {
    struct stat a, b;
    if (stat(native, &a) || stat(usb, &b) || !S_ISREG(a.st_mode) ||
        !S_ISREG(b.st_mode) || a.st_size <= 0 || a.st_size != b.st_size)
        return -1;
    int fd1 = open(native, O_RDONLY), fd2 = open(usb, O_RDONLY);
    if (fd1 < 0 || fd2 < 0) {
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        return -1;
    }
    const size_t chunk = 64 * 1024;
    unsigned char *both = malloc(chunk * 2);
    if (!both) {
        printf("[dlc-redirect] COMPARE FAILED: unable to allocate %zu-byte buffers\n", chunk * 2);
        close(fd1);
        close(fd2);
        return -1;
    }
    unsigned char *x = both, *y = both + chunk;
    int rc = 0;
    for (;;) {
        ssize_t n = read(fd1, x, chunk);
        if (n < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (!n) break;
        size_t done = 0;
        while (done < (size_t)n) {
            ssize_t m = read(fd2, y + done, (size_t)n - done);
            if (m < 0 && errno == EINTR) continue;
            if (m <= 0) { rc = -1; break; }
            done += (size_t)m;
        }
        if (rc || memcmp(x, y, (size_t)n)) { rc = -1; break; }
    }
    free(both);
    close(fd1);
    close(fd2);
    return rc;
}

static int redirect_copy_meta(const char *native, const char *source, const char *filename) {
    char from[PATH_MAX], to[PATH_MAX];
    snprintf(from, sizeof(from), "%s/%s", native, filename);
    snprintf(to, sizeof(to), "%s/%s", source, filename);
    struct stat st, target_stat;
    if (lstat(from, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 65536)
        return -1;
    /* Refuse an unexpected staging symlink/device; write_file must never
     * follow it into another directory or modify the native SSD. */
    if (!lstat(to, &target_stat) && !S_ISREG(target_stat.st_mode))
        return -1;
    unsigned char *buf = malloc((size_t)st.st_size);
    if (!buf) return -1;
    int fd = open(from, O_RDONLY);
    if (fd < 0) { free(buf); return -1; }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    int rc = done == (size_t)st.st_size ? write_file(to, buf, done, 0770) : -1;
    free(buf);
    return rc;
}

/* Can be called after daemon restart: does not rely on a process-local mount
 * counter. Never force-unmount; preserve unknown/foreign layers. */
/* --- One-click native DLC registration -> persistent USB offload ---------
 * This is independent of the historical RE7 and GoW reversible experiments.
 * No AppInstallAll and no direct SQLite modification. Each DLC is treated alone.
 * The native ac.pkg is replaced by an empty stub only after byte comparison,
 * journaling and successful USB nullfs mount, then its held copy is unlinked.
 * The journal is NOT deleted after success: it is required after a PS5 reboot.
 */
typedef struct {
    char cid[37], native[PATH_MAX], stage[PATH_MAX], usb[PATH_MAX];
} offload_record_t;

static void offload_journal_path(const char *cid, char *dst, size_t cap) {
    snprintf(dst,cap,STATE_DIR "/offload_%.36s.txt",cid);
}
static int offload_cid_valid(const char *cid) {
    if (!cid || strlen(cid)!=36 || strncmp(cid+7,"CUSA",4) ||
        cid[16]!='_' || cid[17]!='0' || cid[18]!='0' || cid[19]!='-') return 0;
    for (int i=0;i<36;i++) {
        char c=cid[i];
        if (!(c=='_' || c=='-' || (c>='0'&&c<='9') || (c>='A'&&c<='Z'))) return 0;
    }
    return 1;
}
static int offload_record_valid(const offload_record_t *r) {
    if (!offload_cid_valid(r->cid)) return 0;
    char label[17], tid[10], expected[PATH_MAX];
    snprintf(label,sizeof(label),"%.16s",r->cid+20);
    snprintf(tid,sizeof(tid),"%.9s",r->cid+7);
    snprintf(expected,sizeof(expected),HOLD_DIR "/offload/%s/%s",tid,label);
    if (strcmp(expected,r->stage)) return 0;
    if (strncmp(r->usb,"/mnt/usb",8) || !strstr(r->usb,"/ps4/") ||
        !strstr(r->usb,"/addcont/") || strcmp(r->usb+strlen(r->usb)-7,"/ac.pkg")) return 0;
    char rel[64]; snprintf(rel,sizeof(rel),"/user/addcont/%s/%s",tid,label);
    if (strcmp(r->native,rel)) {
        int good=0;
        for (int i=0;i<8;i++) {
            char path[PATH_MAX];
            snprintf(path,sizeof(path),"/mnt/ext%d%s",i,rel);
            if (!strcmp(path,r->native)) good=1;
        }
        if (!good) return 0;
    }
    return 1;
}
static int offload_read(const char *journal,offload_record_t *r) {
    memset(r,0,sizeof(*r));
    FILE *f=fopen(journal,"r"); if (!f) return -1;
    char *items[]={r->cid,r->native,r->stage,r->usb};
    size_t cap[]={sizeof(r->cid),sizeof(r->native),sizeof(r->stage),sizeof(r->usb)};
    char line[PATH_MAX+128];int good=1;
    for (int i=0;i<4;i++) {
        if (!fgets(line,sizeof(line),f)) {good=0;break;}
        size_t n=strlen(line);
        if (!n || line[n-1]!='\n' || n>cap[i]) {good=0;break;}
        line[--n]=0; memcpy(items[i],line,n+1);
    }
    fclose(f);
    return good && offload_record_valid(r) ? 0:-1;
}
static int offload_write(const offload_record_t *r) {
    char journal[PATH_MAX],tmp[PATH_MAX];
    offload_journal_path(r->cid,journal,sizeof(journal));
    snprintf(tmp,sizeof(tmp),"%s.tmp",journal);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd<0) return -1;
    char data[PATH_MAX*3+80];
    int n=snprintf(data,sizeof(data),"%s\n%s\n%s\n%s\n",r->cid,r->native,r->stage,r->usb);
    int ok=n>0 && (size_t)n<sizeof(data);
    if (ok) {
        int wrote=0;
        while (wrote<n) {
            ssize_t w=write(fd,data+wrote,(size_t)(n-wrote));
            if (w<0 && errno==EINTR) continue;
            if (w<=0) {ok=0;break;}
            wrote+=(int)w;
        }
    }
    if (ok && fsync(fd)) ok=0;
    close(fd);
    if (!ok || rename(tmp,journal)) {unlink(tmp);return -1;}
    int dfd=open(STATE_DIR,O_RDONLY);if (dfd>=0){fsync(dfd);close(dfd);}
    return 0;
}
/* Move the huge file to a sibling, not under native/, so it remains accessible
 * even after native/ is covered by nullfs. Never cross filesystems. */
static int offload_held(const offload_record_t *r, char *held,size_t cap) {
    char parent[PATH_MAX];snprintf(parent,sizeof(parent),"%s",r->native);
    char *end=strrchr(parent,'/');if(!end)return -1;*end=0;
    snprintf(held,cap,"%s/.ps4mount-held/%s.pkg",parent,r->cid+20);
    return 0;
}
static int offload_confirm_overlay(const offload_record_t *r) {
    char pkg[PATH_MAX],target[PATH_MAX];
    snprintf(pkg,sizeof(pkg),"%s/ac.pkg",r->native);
    ssize_t n=readlink(pkg,target,sizeof(target)-1);
    if(n<=0)return -1;target[n]=0;
    return strcmp(target,r->usb) || top_layer(r->native,r->stage)!=1 ||
           owned_layer_count(r->native,r->stage)!=1 ? -1 : 0;
}
static int offload_reconnect(const offload_record_t *r,int finish_held) {
    char pkg[PATH_MAX],held[PATH_MAX];
    struct stat st;
    snprintf(pkg,sizeof(pkg),"%s/ac.pkg",r->native);
    offload_held(r,held,sizeof(held));
    struct stat held_st, pkg_st;
    if (!lstat(held,&held_st) && S_ISREG(held_st.st_mode) &&
        (lstat(pkg,&pkg_st) || (S_ISREG(pkg_st.st_mode) && pkg_st.st_size == 0)) &&
        top_layer(r->native,r->stage) == 0) {
        /* Journal was created but deletion was not committed. Always prefer
         * restoring the huge original over leaving an accidental empty stub. */
        if (!lstat(pkg,&pkg_st) && unlink(pkg)) return -1;
        if (rename(held,pkg)) return -1;
        printf("[offload] %s interrupted staging rolled back to full native PKG\n",r->cid);
        return -1;
    }
    if (stat(r->usb,&st) || !S_ISREG(st.st_mode) || !st.st_size) {
        printf("[offload] %s USB PKG unavailable or empty: %s\n",r->cid,r->usb);
        return -1;
    }
    /* USB mount points can be reused for another disk; verify that the saved
     * file still has the same content ID, not just the same pathname. */
    pkg_t pkg_verify={0};
    if (pkg_open(&pkg_verify,r->usb)) {
        printf("[offload] %s USB package could not be parsed\n",r->cid);
        return -1;
    }
    int same_cid=!strcmp(pkg_verify.content_id,r->cid);
    pkg_close(&pkg_verify);
    if (!same_cid) {
        printf("[offload] %s USB path now points to another package; not mounting\n",r->cid);
        return -1;
    }
    if (top_layer(r->native,r->stage)==1) {
        if (offload_confirm_overlay(r))return -1;
    } else {
        if (layers_on(r->native) || dead_layer(r->native)) return -1;
        if (stat(r->usb,&st) || !S_ISREG(st.st_mode) || !st.st_size) {
            printf("[offload] %s USB absent; cannot activate blank SSD stub\n",r->cid);
            return -1;
        }
        if (lstat(pkg,&st) || !S_ISREG(st.st_mode) || st.st_size) {
            printf("[offload] %s native stub missing/invalid; not mounting\n",r->cid);
            return -1;
        }
        if (nullfs_mount(r->stage,r->native) || offload_confirm_overlay(r)) return -1;
    }
    if (finish_held && !lstat(held,&st) && S_ISREG(st.st_mode)) {
        /* A previous run was interrupted after mount. Only finalize if DB is
         * registered and our USB overlay is validated again. */
        const char *id=r->cid;int registered=-1;
        db_dlc_registration(&id,1,&registered);
        if (registered!=1 || offload_confirm_overlay(r))return -1;
        if (unlink(held))return -1;
        printf("[offload] %s interrupted transaction completed: held SSD file deleted\n",id);
    }
    printf("[offload] ACTIVE %s: native ac.pkg is 0 bytes under the overlay; USB=%s\n",r->cid,r->usb);
    return 0;
}
static int offload_locate_native(const char *tid,const char *label,char *dst,size_t cap) {
    char path[PATH_MAX]; struct stat st;
    /* Prefer real native directories (especially the expansion SSD); skip an
     * external nullfs layer masquerading as an installed DLC. */
    for (int i=0;i<9;i++) {
        if (i<8)snprintf(path,sizeof(path),"/mnt/ext%d/user/addcont/%s/%s",i,tid,label);
        else snprintf(path,sizeof(path),"/user/addcont/%s/%s",tid,label);
        if (layers_on(path) || dead_layer(path))continue;
        char pkg[PATH_MAX];snprintf(pkg,sizeof(pkg),"%s/ac.pkg",path);
        if(!lstat(pkg,&st) && S_ISREG(st.st_mode) && st.st_size>0) {
            snprintf(dst,cap,"%s",path);return 0;
        }
    }
    return -1;
}
/* Fallback for FW 11.40 GetInstallStatus=0x80a30003. A DB row by itself
 * does not prove the installer finished copying. Require 15 consecutive
 * one-second stable observations of the exact native PKG size and timestamps,
 * plus required metadata and an unchanged registration. Offload still performs
 * the authoritative byte-for-byte SSD-vs-USB comparison before moving files.
 * An existing zero-byte stub is handled by journal recovery, not installation. */
static int offload_wait_native_ready(const dlc_src_t *dlc, const char *tid, int status_unavailable) {
    const char *cid = dlc->pkg.content_id;
    char journal[PATH_MAX], native[PATH_MAX], pkg[PATH_MAX], metadata[PATH_MAX];
    struct stat usb, current, previous = {0}, m;
    int stable = 0, logged = -1;
    if (stat(dlc->path,&usb) || !S_ISREG(usb.st_mode) || usb.st_size <= 0) return -1;
    offload_journal_path(cid,journal,sizeof(journal));
    if (exists(journal)) {
        offload_record_t rec;
        if (offload_read(journal,&rec) || strcmp(rec.usb,dlc->path)) {
            printf("[offload] %s cannot resume invalid/different-USB journal\n",cid);
            return -1;
        }
        snprintf(pkg,sizeof(pkg),"%s/ac.pkg",rec.native);
        if (top_layer(rec.native,rec.stage)==1 && !offload_confirm_overlay(&rec)) {
            printf("[offload] %s USB overlay already active; validating journal\n",cid);
            return 0;
        }
        if (top_layer(rec.native,rec.stage)==0 && !lstat(pkg,&current) &&
            S_ISREG(current.st_mode) && current.st_size==0) {
            printf("[offload] %s existing empty native stub and journal; validating remount\n",cid);
            return 0; /* offload_one performs the journal/USB/DB checks */
        }
        /* If the original PKG was recreated, wait for it to settle. */
    }
    for (int sec=0; sec<180; sec++) {
        if (g_stop || title_running(tid)) return -1;
        int registered=-1;
        db_dlc_registration(&cid,1,&registered);
        int present = registered==1 &&
            !offload_locate_native(tid,cid+20,native,sizeof(native));
        if (present) {
            snprintf(pkg,sizeof(pkg),"%s/ac.pkg",native);
            present = !lstat(pkg,&current) && S_ISREG(current.st_mode) &&
                      current.st_size==usb.st_size && !layers_on(native) && !dead_layer(native);
        }
        const char *names[]={"ac.json","ac.pbm","ac.pbm.backup"};
        for(int j=0; present && j<3; j++) {
            snprintf(metadata,sizeof(metadata),"%s/%s",native,names[j]);
            if (lstat(metadata,&m) || !S_ISREG(m.st_mode) || m.st_size<=0) present=0;
        }
        if (present && stable &&
            current.st_dev==previous.st_dev && current.st_ino==previous.st_ino &&
            current.st_size==previous.st_size && current.st_mtime==previous.st_mtime &&
            current.st_ctime==previous.st_ctime) stable++;
        else stable=present?1:0;
        if (present) previous=current;
        if (sec / 10 != logged) {
            logged=sec/10;
            printf("[dlc-verify] %s %s sec=%d db=%d native_complete=%d stable=%d/15 size=%llu expected=%llu\n",
                   cid,status_unavailable?"STATUS_FALLBACK":"NORMAL",sec,registered,present,stable,
                   (unsigned long long)(present?current.st_size:0),
                   (unsigned long long)usb.st_size);
        }
        if (stable>=15) {
            /* There is no reliable native-task completion event on this FW.
             * Recheck after a quiet interval; cannot guarantee OS cache flush. */
            wait_db_quiet(3,12);
            int after=-1;db_dlc_registration(&cid,1,&after);
            struct stat check;
            if(after==1 && !lstat(pkg,&check) && S_ISREG(check.st_mode) &&
               check.st_dev==current.st_dev && check.st_ino==current.st_ino &&
               check.st_size==usb.st_size && check.st_mtime==current.st_mtime &&
               check.st_ctime==current.st_ctime && !layers_on(native)) {
                printf("[dlc-verify] %s READY: DB=1, native file+metadata stable, native=%s; byte compare next\n",cid,native);
                return 0;
            }
            stable=0;
        }
        pump_events(1000);
    }
    printf("[dlc-verify] %s TIMEOUT: native file/metadata/DB not stable; SSD preserved\n",cid);
    return -1;
}

static int offload_one(const dlc_src_t *dlc,const char *tid) {
    const char *cid=dlc->pkg.content_id;
    char journal[PATH_MAX],pkg[PATH_MAX],held[PATH_MAX],root[32];
    offload_record_t rec={0};struct stat st;
    if (!offload_cid_valid(cid) || strncmp(cid+7,tid,9) || title_running(tid))return -1;
    offload_journal_path(cid,journal,sizeof(journal));
    if (exists(journal)) {
        if (offload_read(journal,&rec) || strcmp(rec.usb,dlc->path)) {
            printf("[offload] %s invalid/stale journal or different USB device; no deletion\n",cid);
            return -1;
        }
        /* If this title was unregistered then reinstalled, the installer may
         * have recreated the large native PKG. Re-run verified offload rather
         * than treating a stale zero-stub journal as current. */
        char oldpkg[PATH_MAX];struct stat oldst;
        snprintf(oldpkg,sizeof(oldpkg),"%s/ac.pkg",rec.native);
        if (top_layer(rec.native,rec.stage)==0 && !lstat(oldpkg,&oldst) &&
            S_ISREG(oldst.st_mode) && oldst.st_size>0) {
            printf("[offload] %s native file recreated after reinstallation; refreshing journal\n",cid);
            if (unlink(journal))return -1;
        } else return offload_reconnect(&rec,1);
    }
    if (find_root(tid,root,sizeof(root)) || !is_mountpoint(root))return -1;
    snprintf(rec.cid,sizeof(rec.cid),"%s",cid);
    snprintf(rec.usb,sizeof(rec.usb),"%s",dlc->path);
    snprintf(rec.stage,sizeof(rec.stage),HOLD_DIR "/offload/%s/%s",tid,cid+20);
    if (offload_locate_native(tid,cid+20,rec.native,sizeof(rec.native))) {
        printf("[offload] %s no native ac.pkg found after successful DB registration\n",cid);
        return -1;
    }
    snprintf(pkg,sizeof(pkg),"%s/ac.pkg",rec.native);
    offload_held(&rec,held,sizeof(held));
    if (!lstat(held,&st)) {printf("[offload] %s stale held file: manual recovery needed\n",cid);return -1;}
    if (redirect_same_file_bytes(pkg,rec.usb)) {
        printf("[offload] %s SSD/USB PKG differs or IO error; preserving SSD\n",cid);
        return -1;
    }
    struct stat matched;
    if (lstat(pkg,&matched) || !S_ISREG(matched.st_mode) || matched.st_size<=0) {
        printf("[offload] %s native PKG changed after comparison; preserving SSD\n",cid);
        return -1;
    }
    printf("[offload] %s NATIVE/USB MATCH size=%llu native=%s; preparing redirect\n",
           cid,(unsigned long long)matched.st_size,rec.native);
    mkdirs(rec.stage,0770);
    const char *meta[]={"ac.json","ac.pbm","ac.pbm.backup"};
    for (int i=0;i<3;i++) if (redirect_copy_meta(rec.native,rec.stage,meta[i]))return -1;
    char link[PATH_MAX];snprintf(link,sizeof(link),"%s/ac.pkg",rec.stage);
    if (!lstat(link,&st) && !S_ISLNK(st.st_mode))return -1;
    if ((!lstat(link,&st) && unlink(link)) || symlink(rec.usb,link))return -1;
    if (layers_on(rec.native) || dead_layer(rec.native) || !offload_record_valid(&rec))return -1;
    /* Write-ahead journal must be durable BEFORE rename / stub creation. */
    if (offload_write(&rec)) {
        printf("[offload] %s cannot persist journal; SSD untouched\n",cid);return -1;
    }
    int moved=0, stub=0, mounted=0;
    char dir[PATH_MAX];snprintf(dir,sizeof(dir),"%s",held);
    char *slash=strrchr(dir,'/');if(!slash)goto rollback;*slash=0;
    mkdirs(dir,0770);
    if(rename(pkg,held))goto rollback;
    moved=1;
    int fd=open(pkg,O_CREAT|O_EXCL|O_WRONLY,0600);
    if (fd<0)goto rollback;
    stub=1;
    if (fsync(fd)) {close(fd);goto rollback;}
    close(fd);
    if(nullfs_mount(rec.stage,rec.native))goto rollback;
    mounted=1;
    if(offload_confirm_overlay(&rec))goto rollback;
    /* Only now is deletion allowed. Native PKG still has a zero-size entry
     * under nullfs; the huge original lives at held until this unlink. */
    if(unlink(held)) {
        printf("[offload] %s mounted, but held-file unlink failed; retry on next mount\n",cid);
        return -1;
    }
    printf("[offload] DONE %s SSD original unlinked: logical_bytes=%llu allocated_bytes_estimate=%llu native_stub=0 usb=%s\n",
           cid,(unsigned long long)matched.st_size,
           (unsigned long long)matched.st_blocks*512ULL,rec.usb);
    return 0;
rollback:
    printf("[offload] %s operation failed; attempting rollback errno=%d\n",cid,errno);
    if(mounted && top_layer(rec.native,rec.stage)==1) {
        if(unmount(rec.native,0)) {
            printf("[offload] %s rollback blocked by mounted layer; journal preserved\n",cid);
            return -1;
        }
    }
    if(stub && unlink(pkg))return -1;
    if(moved && rename(held,pkg)) {
        printf("[offload] %s CRITICAL: held copy preserved at %s\n",cid,held);
        return -1;
    }
    unlink(journal);
    return -1;
}
static int offload_resume_title(const char *tid) {
    if (title_running(tid))return -1;
    DIR *dir=opendir(STATE_DIR);if(!dir)return -1;
    int result=0;struct dirent *e;
    while ((e=readdir(dir))) {
        if (strncmp(e->d_name,"offload_",8) || strlen(e->d_name)!=8+36+4 ||
            strcmp(e->d_name+44,".txt"))continue;
        char journal[PATH_MAX];snprintf(journal,sizeof(journal),STATE_DIR "/%s",e->d_name);
        offload_record_t rec;
        if (offload_read(journal,&rec)) {printf("[offload] invalid journal %s\n",journal);result=-1;continue;}
        if(strncmp(rec.cid+7,tid,9))continue;
        const char *id=rec.cid;int registered=-1;
        db_dlc_registration(&id,1,&registered);
        if (registered!=1) {
            printf("[offload] %s no current DB registration; remount postponed\n",id);result=-1;continue;
        }
        if (offload_reconnect(&rec,1))result=-1;
    }
    closedir(dir);
    return result;
}
static void offload_resume_all(void) {
    DIR *d=opendir(STATE_DIR);if(!d)return;
    char (*tids)[10]=calloc(MAX_TITLES,sizeof(*tids));int n=0;
    struct dirent *e;
    while (tids && (e=readdir(d)) && n<MAX_TITLES) {
        if (strncmp(e->d_name,"offload_",8) || strlen(e->d_name)!=48 ||
            strcmp(e->d_name+44,".txt"))continue;
        char tid[10];snprintf(tid,sizeof(tid),"%.9s",e->d_name+8+7);
        int existing=0;for(int i=0;i<n;i++)if(!strcmp(tids[i],tid))existing=1;
        if(!existing)snprintf(tids[n++],10,"%s",tid);
    }
    closedir(d);
    for (int i=0;i<n;i++) if (!title_running(tids[i])) {
        /* The game may be manually unmounted but its DLC stays registered.
         * offload_resume_title itself checks each DLC database registration. */
        printf("[offload] restore USB DLC overlay for %s (base may be disabled)\n",tids[i]);
        offload_resume_title(tids[i]);
    }
    free(tids);
}
int app_restore_native_dlc(const char *tid, char *msg, size_t cap) {
    if (strcmp(tid, REDIRECT_TID)) {
        snprintf(msg, cap, "Teste restrito a " REDIRECT_TID);
        return -1;
    }
    if (redirect_busy() || title_running(tid)) {
        snprintf(msg, cap, "Feche o jogo e aguarde as verificacoes terminarem");
        return -1;
    }
    int restored = 0, failed = 0;
    char native[PATH_MAX], source[PATH_MAX];
    for (int i = REDIRECT_COUNT - 1; i >= 0; --i) {
        redirect_paths(i, native, sizeof(native), source, sizeof(source));
        int top = top_layer(native, source);
        int count = owned_layer_count(native, source);
        if (!top && count == 0) continue;
        if (top != 1 || count != 1) {
            printf("[dlc-redirect] RESTORE REFUSED %s: unknown/stacked mount top=%d count=%d\n",
                   g_redirect_labels[i], top, count);
            failed++;
            continue;
        }
        if (unmount(native, 0)) {
            printf("[dlc-redirect] RESTORE FAILED %s errno=%d (%s)\n",
                   g_redirect_labels[i], errno, strerror(errno));
            failed++;
            continue;
        }
        char pkg[PATH_MAX];
        snprintf(pkg, sizeof(pkg), "%s/ac.pkg", native);
        struct stat st;
        if (lstat(pkg, &st) || !S_ISREG(st.st_mode)) {
            printf("[dlc-redirect] RESTORE CHECK FAILED %s: original ac.pkg not visible\n",
                   g_redirect_labels[i]);
            failed++;
            continue;
        }
        restored++;
        printf("[dlc-redirect] RESTORED %d/%d %s native SSD ac.pkg=%llu bytes\n",
               restored, REDIRECT_COUNT, g_redirect_labels[i],
               (unsigned long long)st.st_size);
    }
    redirect_status(failed ? 4 : 5, restored);
    printf("[dlc-redirect] RESTORE FINISH restored=%d failed=%d native DB untouched\n", restored, failed);
    snprintf(msg, cap, "Restaurados %d DLC(s), falhas %d (veja o log)", restored, failed);
    return failed ? -1 : 0;
}

/* Worker is started only after the 17 DB registrations are checked on the
 * main thread. All 17 USB IDs are mapped first without mounting anything.
 * Then for each item: compare -> stage -> mount -> verify -> next item.
 * A failure reverses mounts owned by this run, in reverse order. */
static void *redirect_all_worker(void *ignored) {
    (void)ignored;
    title_src_t *t = NULL;
    char (*usb)[PATH_MAX] = NULL;
    char root[32] = {0};
    int mounted[REDIRECT_COUNT] = {0};
    int failed = 0, opened = 0;
    printf("[dlc-redirect] WORKER START sequential test %d DLCs; no SSD writes\n", REDIRECT_COUNT);
    fflush(stdout);
    redirect_status(1, 0);
    __atomic_store_n(&g_redirect_current, 0, __ATOMIC_RELEASE);
    /* Avoid large worker stack frames (also protects the PKG reader). */
    t = calloc(1, sizeof(*t));
    usb = calloc(REDIRECT_COUNT, sizeof(*usb));
    if (!t || !usb) {
        printf("[dlc-redirect] FAILED allocating worker state\n");
        failed = 1;
        goto out;
    }
    if (find_root(REDIRECT_TID, root, sizeof(root)) ||
        strncmp(root, "/mnt/usb", 8) || !is_mountpoint(root) ||
        src_open(t, root, REDIRECT_TID)) {
        printf("[dlc-redirect] FAILED: RE7 USB missing/unreadable; no overlays\n");
        failed = 1;
        goto out;
    }
    opened = 1;
    printf("[dlc-redirect] WORKER USB opened root=%s found=%d expected=%d\n",
           root, t->ndlc, REDIRECT_COUNT);
    if (t->ndlc != REDIRECT_COUNT) {
        printf("[dlc-redirect] FAILED: expected %d USB DLCs, found %d\n",
               REDIRECT_COUNT, t->ndlc);
        failed = 1;
        goto out;
    }
    for (int i = 0; i < REDIRECT_COUNT; i++) {
        static const char prefix[] = "EP0102-CUSA03842_00-";
        int which = -1;
        for (int j = 0; j < t->ndlc; j++)
            if (!strncmp(t->dlc[j].pkg.content_id, prefix, sizeof(prefix)-1) &&
                !strcmp(t->dlc[j].pkg.content_id + sizeof(prefix)-1, g_redirect_labels[i])) {
                which = j;
                break;
            }
        if (which < 0) {
            printf("[dlc-redirect] FAILED USB DLC not found %d/%d %s; no overlays\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        snprintf(usb[i], sizeof(usb[i]), "%s", t->dlc[which].path);
    }
    for (int i = 0; i < REDIRECT_COUNT; i++) {
        char native[PATH_MAX], source[PATH_MAX], pkg[PATH_MAX], link[PATH_MAX];
        char actual[PATH_MAX];
        struct stat st;
        const char *meta[] = {"ac.json", "ac.pbm", "ac.pbm.backup"};
        redirect_paths(i, native, sizeof(native), source, sizeof(source));
        __atomic_store_n(&g_redirect_current, i + 1, __ATOMIC_RELEASE);
        redirect_status(1, i);
        printf("[dlc-redirect] STEP %d/%d %s: VERIFY START\n",
               i + 1, REDIRECT_COUNT, g_redirect_labels[i]);
        fflush(stdout);
        snprintf(pkg, sizeof(pkg), "%s/ac.pkg", native);
        if (lstat(pkg, &st) || !S_ISREG(st.st_mode) ||
            layers_on(native) || dead_layer(native) ||
            owned_layer_count(native, source) != 0 || redirect_title_running() ||
            !is_mountpoint(root)) {
            printf("[dlc-redirect] STEP %d/%d FAILED native/USB/mount preflight: %s\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        if (redirect_same_file_bytes(pkg, usb[i])) {
            printf("[dlc-redirect] STEP %d/%d MISMATCH/IO ERROR: %s\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        printf("[dlc-redirect] STEP %d/%d MATCH: %s\n",
               i+1, REDIRECT_COUNT, g_redirect_labels[i]);
        redirect_status(2, i);
        printf("[dlc-redirect] STEP %d/%d %s: STAGING + MOUNT START\n",
               i+1, REDIRECT_COUNT, g_redirect_labels[i]);
        fflush(stdout);
        mkdirs(source, 0770);
        for (int j = 0; j < 3; j++) {
            if (redirect_copy_meta(native, source, meta[j])) {
                printf("[dlc-redirect] STEP %d/%d FAILED staging %s/%s\n",
                       i+1, REDIRECT_COUNT, g_redirect_labels[i], meta[j]);
                failed = 1;
                goto out;
            }
        }
        snprintf(link, sizeof(link), "%s/ac.pkg", source);
        if (!lstat(link, &st) && !S_ISLNK(st.st_mode)) {
            printf("[dlc-redirect] STEP %d/%d FAILED foreign staging link %s\n",
                   i+1, REDIRECT_COUNT, link);
            failed = 1;
            goto out;
        }
        if ((!lstat(link, &st) && unlink(link)) || symlink(usb[i], link)) {
            printf("[dlc-redirect] STEP %d/%d FAILED staging USB symlink %s\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        if (redirect_title_running() || !is_mountpoint(root) ||
            layers_on(native) || dead_layer(native)) {
            printf("[dlc-redirect] STEP %d/%d FAILED mount safety check %s\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        if (nullfs_mount(source, native)) {
            printf("[dlc-redirect] STEP %d/%d FAILED nullfs %s errno=%d\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i], errno);
            failed = 1;
            goto out;
        }
        mounted[i] = 1; /* always roll back if the following check fails */
        ssize_t n = readlink(pkg, actual, sizeof(actual)-1);
        if (n > 0) actual[n] = 0;
        if (n <= 0 || strcmp(actual, usb[i]) || top_layer(native, source) != 1 ||
            owned_layer_count(native, source) != 1) {
            printf("[dlc-redirect] STEP %d/%d FAILED overlay validation %s\n",
                   i+1, REDIRECT_COUNT, g_redirect_labels[i]);
            failed = 1;
            goto out;
        }
        printf("[dlc-redirect] ACTIVE %d/%d %s ac.pkg -> %s; SSD remains intact\n",
               i+1, REDIRECT_COUNT, g_redirect_labels[i], actual);
        redirect_status(1, i+1);
        fflush(stdout);
    }
    redirect_status(3, REDIRECT_COUNT);
    __atomic_store_n(&g_redirect_current, REDIRECT_COUNT, __ATOMIC_RELEASE);
    printf("[dlc-redirect] ALL ACTIVE 17/17; DB unchanged; original SSD files preserved\n");
out:
    if (failed) {
        int rollback_failed = 0;
        printf("[dlc-redirect] ERROR: restoring overlays already mounted in this run\n");
        for (int i = REDIRECT_COUNT-1; i >= 0; --i) {
            if (!mounted[i]) continue;
            char native[PATH_MAX], source[PATH_MAX];
            redirect_paths(i, native, sizeof(native), source, sizeof(source));
            if (top_layer(native, source) != 1 || owned_layer_count(native, source) != 1 ||
                unmount(native, 0)) {
                printf("[dlc-redirect] ROLLBACK FAILED %s; restore only when game closed\n", g_redirect_labels[i]);
                rollback_failed++;
            } else {
                printf("[dlc-redirect] ROLLBACK %s native SSD visible\n", g_redirect_labels[i]);
            }
        }
        printf("[dlc-redirect] FAILED; rollback_failures=%d; verify log before playing\n",
               rollback_failed);
        redirect_status(4, 0);
    }
    if (opened) src_close(t);
    free(t);
    free(usb);
    printf("[dlc-redirect] WORKER EXIT status=%s\n", failed ? "FAILED" : "OK");
    fflush(stdout);
    __atomic_store_n(&g_redirect_job_active, 0, __ATOMIC_RELEASE);
    return NULL;
}

int app_redirect_native_dlc(const char *tid, char *msg, size_t cap) {
    if (strcmp(tid, REDIRECT_TID)) {
        snprintf(msg, cap, "Teste restrito ao Resident Evil 7 (" REDIRECT_TID ")");
        return -1;
    }
    if (redirect_busy() || title_running(tid) || redirect_mounted()) {
        snprintf(msg, cap, "Feche RE7 e finalize outros trabalhos/overlays antes do teste");
        return -1;
    }
    if (!db_snapshot_healthy() || !db_is_registered(tid)) {
        snprintf(msg, cap, "Jogo base nao registrado ou DB indisponivel");
        return -1;
    }
    /* Do ALL SQLite lookups on the main thread, before starting the worker.
     * SQLite is compiled SQLITE_THREADSAFE=0 in build.sh; never run concurrent
     * SQLite queries from the worker and the browser/catalog main loop. */
    char id_storage[REDIRECT_COUNT][37];
    const char *ids[REDIRECT_COUNT];
    int db[REDIRECT_COUNT];
    for (int i = 0; i < REDIRECT_COUNT; ++i) {
        snprintf(id_storage[i], sizeof(id_storage[i]), "EP0102-CUSA03842_00-%s", g_redirect_labels[i]);
        ids[i] = id_storage[i];
    }
    db_dlc_registration(ids, REDIRECT_COUNT, db);
    for (int i = 0; i < REDIRECT_COUNT; ++i) {
        if (db[i] != 1) {
            printf("[dlc-redirect] REFUSED: DLC not registered %s db_state=%d\n", ids[i], db[i]);
            snprintf(msg, cap, "Os 17 DLCs precisam estar registrados no addcont.db");
            return -1;
        }
    }
    /* Any foreign mount on one of the 17 targets prevents another test. */
    for (int i = 0; i < REDIRECT_COUNT; i++) {
        char native[PATH_MAX], source[PATH_MAX];
        redirect_paths(i, native, sizeof(native), source, sizeof(source));
        if (layers_on(native) || dead_layer(native) || owned_layer_count(native, source) != 0) {
            snprintf(msg, cap, "Um DLC ja tem montagem ou camada desconhecida; restaure antes");
            return -1;
        }
    }
    __atomic_store_n(&g_redirect_job_active, 1, __ATOMIC_RELEASE);
    redirect_status(1, 0);
    __atomic_store_n(&g_redirect_current, 0, __ATOMIC_RELEASE);
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        int stack_rc = pthread_attr_setstacksize(&attr, 512 * 1024);
        if (stack_rc) {
            printf("[dlc-redirect] FAILED configuring worker stack rc=%d\n", stack_rc);
            pthread_attr_destroy(&attr);
            rc = stack_rc;
        }
    }
    if (rc) {
        __atomic_store_n(&g_redirect_job_active, 0, __ATOMIC_RELEASE);
        redirect_status(4, 0);
        snprintf(msg, cap, "Nao foi possivel preparar thread (pthread %d)", rc);
        return -1;
    }
    pthread_t worker;
    rc = pthread_create(&worker, &attr, redirect_all_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        __atomic_store_n(&g_redirect_job_active, 0, __ATOMIC_RELEASE);
        redirect_status(4, 0);
        snprintf(msg, cap, "Nao foi possivel iniciar verificacao dos DLCs (pthread %d)", rc);
        return -1;
    }
    pthread_detach(worker);
    printf("[dlc-redirect] QUEUED SEQUENTIAL verify+mount each of 17 native DLCs of %s; SSD untouched\n", tid);
    snprintf(msg, cap, "Verificando e montando 1 DLC por vez (17); acompanhe progresso");
    return 0;
}

/* v0.4.18: single GoW DLC reversible experiment. No delete, no DB writes.
 * A durable journal is created BEFORE changing the native ac.pkg name. The
 * native file remains on its SSD under ac.pkg.ps4mount-held until restoration.
 * The active directory is covered with nullfs sourced from USB metadata. */
#define PILOT_TID "CUSA34384"
#define PILOT_JOURNAL HOLD_DIR "/pilot-gow-one.txt"
#define PILOT_SOURCE_ROOT HOLD_DIR "/pilot-gow-one/"
#define PILOT_HELD_SUFFIX ".ps4mount-held"
static int g_pilot_job;
static char g_pilot_cid[37];
static int g_pilot_stage; /* 0 idle, 1 validating, 2 overlaying, 3 active, 4 failed */

typedef struct { char cid[37], native[PATH_MAX], source[PATH_MAX], usb[PATH_MAX]; } pilot_record_t;

static int pilot_cid_valid(const char *cid) {
    if (strlen(cid) != 36 || strncmp(cid+7, PILOT_TID, 9) ||
        cid[16] != '_' || cid[17] != '0' || cid[18] != '0' || cid[19] != '-') return 0;
    for (int i=0; i<36; ++i) {
        char c=cid[i];
        if (!((c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_')) return 0;
    }
    return 1;
}
static int pilot_record_valid(const pilot_record_t *r) {
    if (!pilot_cid_valid(r->cid)) return 0;
    char suffix[40]; snprintf(suffix, sizeof(suffix), "/%s", r->cid+20);
    const char *roots[] = {
        "/mnt/ext1/user/addcont/" PILOT_TID,
        "/mnt/ext0/user/addcont/" PILOT_TID,
        "/user/addcont/" PILOT_TID
    };
    int known=0;
    for (int i=0; i<3; ++i) {
        char expected[PATH_MAX]; snprintf(expected,sizeof(expected),"%s%s",roots[i],suffix);
        if (!strcmp(expected,r->native)) known=1;
    }
    char source[PATH_MAX]; snprintf(source,sizeof(source),PILOT_SOURCE_ROOT "%s",r->cid+20);
    return known && !strcmp(source,r->source) &&
        !strncmp(r->usb,"/mnt/usb",8) && strstr(r->usb,"/ps4/" PILOT_TID "/addcont/") != NULL &&
        strstr(r->usb,"/ac.pkg") != NULL;
}
static int pilot_read(pilot_record_t *r) {
    memset(r,0,sizeof(*r));
    FILE *f=fopen(PILOT_JOURNAL,"r"); if (!f) return -1;
    char *v[]={r->cid,r->native,r->source,r->usb};
    size_t caps[]={sizeof(r->cid),sizeof(r->native),sizeof(r->source),sizeof(r->usb)};
    char line[PATH_MAX+64];
    int ok=1;
    for (int i=0;i<4;i++) {
        if (!fgets(line,sizeof(line),f)) {ok=0;break;}
        size_t n=strlen(line);
        if (!n || line[n-1]!='\n' || n>caps[i]) {ok=0;break;}
        line[--n]=0;
        memcpy(v[i],line,n+1);
    }
    fclose(f);
    return ok && pilot_record_valid(r) ? 0:-1;
}
static int pilot_write(const pilot_record_t *r) {
    char buf[PATH_MAX*3+64];
    int n=snprintf(buf,sizeof(buf),"%s\n%s\n%s\n%s\n",r->cid,r->native,r->source,r->usb);
    if (n<=0 || (size_t)n>=sizeof(buf)) return -1;
    mkdirs(HOLD_DIR,0770);
    char tmp[PATH_MAX]; snprintf(tmp,sizeof(tmp),"%s.tmp",PILOT_JOURNAL);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if (fd<0) return -1;
    size_t written=0;
    while (written<(size_t)n) {
        ssize_t got=write(fd,buf+written,(size_t)n-written);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) break;
        written+=(size_t)got;
    }
    int synced = written==(size_t)n && fsync(fd)==0;
    close(fd);
    if (!synced) {unlink(tmp);return -1;}
    if (rename(tmp,PILOT_JOURNAL)) {unlink(tmp);return -1;}
    int d=open(HOLD_DIR,O_RDONLY);
    if (d>=0) {fsync(d);close(d);}
    return 0;
}
static int pilot_locked(void) {
    return __atomic_load_n(&g_pilot_job,__ATOMIC_ACQUIRE) || exists(PILOT_JOURNAL);
}
static int pilot_title_running(void) {
    int n=getfsstat(NULL,0,MNT_NOWAIT); if (n<=0) return 1;
    struct statfs *mounts=calloc((size_t)n+16,sizeof(*mounts)); if (!mounts) return 1;
    n=getfsstat(mounts,(long)((size_t)(n+16)*sizeof(*mounts)),MNT_NOWAIT);
    int running = n<0;
    for (int i=0;!running && i<n;i++)
        if (!strcmp(mounts[i].f_mntonname,"/mnt/sandbox/pfsmnt/" PILOT_TID "-app0")) running=1;
    free(mounts);return running;
}
static int pilot_restore_internal(int recover) {
    if (!exists(PILOT_JOURNAL)) return 0;
    pilot_record_t r;
    if (pilot_read(&r)) {printf("[dlc-pilot] RESTORE BLOCKED: invalid journal; manual inspection needed\n");return -1;}
    if (pilot_title_running()) {printf("[dlc-pilot] RESTORE BLOCKED: game running\n");return -1;}
    int top=top_layer(r.native,r.source),count=owned_layer_count(r.native,r.source);
    if (top==1 && count==1) {
        if (unmount(r.native,0)) {
            printf("[dlc-pilot] RESTORE BLOCKED unmount errno=%d\n",errno);return -1;
        }
    } else if (top!=0 || count!=0 || layers_on(r.native) || dead_layer(r.native)) {
        printf("[dlc-pilot] RESTORE BLOCKED foreign mount top=%d count=%d\n",top,count);return -1;
    }
    char pkg[PATH_MAX],held[PATH_MAX]; struct stat st;
    snprintf(pkg,sizeof(pkg),"%s/ac.pkg",r.native);
    snprintf(held,sizeof(held),"%s/ac.pkg" PILOT_HELD_SUFFIX,r.native);
    if (!lstat(held,&st) && S_ISREG(st.st_mode)) {
        if (!lstat(pkg,&st)) {printf("[dlc-pilot] RESTORE BLOCKED: target already exists\n");return -1;}
        if (rename(held,pkg)) {printf("[dlc-pilot] RESTORE FAILED rename errno=%d\n",errno);return -1;}
    } else if (lstat(pkg,&st) || !S_ISREG(st.st_mode)) {
        printf("[dlc-pilot] RESTORE BLOCKED: neither original nor backup is valid\n");return -1;
    }
    unlink(PILOT_JOURNAL);
    printf("[dlc-pilot] RESTORED %s native SSD file; recovery=%d DB untouched\n",r.cid,recover);
    __atomic_store_n(&g_pilot_stage,0,__ATOMIC_RELEASE);
    return 0;
}

int app_pilot_restore(const char *tid,const char *cid,char *msg,size_t cap) {
    pilot_record_t r;
    if (strcmp(tid,PILOT_TID) || pilot_read(&r) || strcmp(cid,r.cid) ||
        __atomic_load_n(&g_pilot_job,__ATOMIC_ACQUIRE)) {
        snprintf(msg,cap,"Nenhum piloto correspondente, ou verificacao em andamento");return -1;
    }
    int rc=pilot_restore_internal(0);
    snprintf(msg,cap,rc?"Falha ao restaurar; confira log e mantenha USB":"DLC restaurado no SSD; original preservado");
    return rc;
}

/* The journal identifies the only active pilot; restoration never needs the USB
 * package list. This matters when the drive is disconnected. */
int app_pilot_restore_current(const char *tid,char *msg,size_t cap) {
    pilot_record_t r;
    if (strcmp(tid,PILOT_TID) || pilot_read(&r)) {
        snprintf(msg,cap,"Nenhum piloto valido para restaurar");return -1;
    }
    return app_pilot_restore(tid,r.cid,msg,cap);
}

/* Paths of native installations vary with SSD location; never guess that
 * /user/addcont is the native copy if PS4Mount has placed a symlink there. */
static int pilot_locate_native(const char *label,char *dest,size_t cap) {
    const char *prefixes[]={"/mnt/ext1/user/addcont/" PILOT_TID,
                           "/mnt/ext0/user/addcont/" PILOT_TID,
                           "/user/addcont/" PILOT_TID};
    char candidate[PATH_MAX], pkg[PATH_MAX], held[PATH_MAX]; struct stat st;
    for (int i=0;i<3;i++) {
        snprintf(candidate,sizeof(candidate),"%s/%s",prefixes[i],label);
        if (layers_on(candidate) || dead_layer(candidate)) continue;
        snprintf(pkg,sizeof(pkg),"%s/ac.pkg",candidate);
        snprintf(held,sizeof(held),"%s/ac.pkg" PILOT_HELD_SUFFIX,candidate);
        if (!lstat(pkg,&st) && S_ISREG(st.st_mode) && st.st_size>0 &&
            lstat(held,&st) != 0) {
            snprintf(dest,cap,"%s",candidate);return 0;
        }
    }
    return -1;
}
static void *pilot_worker(void *ignored) {
    (void)ignored;
    char cid[37],root[32]={0};
    snprintf(cid,sizeof(cid),"%s",g_pilot_cid);
    printf("[dlc-pilot] WORKER START %s (one DLC only)\n",cid);fflush(stdout);
    pilot_record_t *r=calloc(1,sizeof(*r));
    title_src_t *t=calloc(1,sizeof(*t));
    int opened=0,ok=0,journal=0;
    if (!r || !t) goto out;
    snprintf(r->cid,sizeof(r->cid),"%s",cid);
    if (find_root(PILOT_TID,root,sizeof(root)) || strncmp(root,"/mnt/usb",8) ||
        !is_mountpoint(root) || src_open(t,root,PILOT_TID)) goto out;
    opened=1;
    int ix=-1;
    for (int i=0;i<t->ndlc;i++) if (!strcmp(t->dlc[i].pkg.content_id,cid)) {ix=i;break;}
    if (ix<0) {printf("[dlc-pilot] USB PKG not found for %s\n",cid);goto out;}
    snprintf(r->usb,sizeof(r->usb),"%s",t->dlc[ix].path);
    snprintf(r->source,sizeof(r->source),PILOT_SOURCE_ROOT "%s",cid+20);
    if (pilot_locate_native(cid+20,r->native,sizeof(r->native))) {
        printf("[dlc-pilot] NO native ac.pkg for %s; install ONLY this DLC natively first\n",cid);goto out;
    }
    char pkg[PATH_MAX],held[PATH_MAX],link[PATH_MAX],actual[PATH_MAX];
    snprintf(pkg,sizeof(pkg),"%s/ac.pkg",r->native);
    snprintf(held,sizeof(held),"%s/ac.pkg" PILOT_HELD_SUFFIX,r->native);
    printf("[dlc-pilot] VERIFY SSD=%s USB=%s\n",pkg,r->usb);fflush(stdout);
    if (redirect_same_file_bytes(pkg,r->usb)) {printf("[dlc-pilot] PKG MISMATCH or read error\n");goto out;}
    printf("[dlc-pilot] MATCH: byte-for-byte native and USB PKG\n");fflush(stdout);
    __atomic_store_n(&g_pilot_stage,2,__ATOMIC_RELEASE);
    mkdirs(r->source,0770);
    const char *meta[]={"ac.json","ac.pbm","ac.pbm.backup"};
    for (int i=0;i<3;i++)
        if (redirect_copy_meta(r->native,r->source,meta[i])) {
            printf("[dlc-pilot] cannot stage %s; original stays unchanged\n",meta[i]);goto out;
        }
    snprintf(link,sizeof(link),"%s/ac.pkg",r->source);
    struct stat st;
    if (!lstat(link,&st) && !S_ISLNK(st.st_mode)) goto out;
    if ((!lstat(link,&st) && unlink(link)) || symlink(r->usb,link)) goto out;
    if (pilot_title_running() || layers_on(r->native) || dead_layer(r->native) ||
        !is_mountpoint(root) || !pilot_record_valid(r) || exists(PILOT_JOURNAL)) goto out;
    /* Persist rollback destination before touching the SSD file. */
    if (pilot_write(r)) {printf("[dlc-pilot] failed to write recovery journal\n");goto out;}
    journal=1;
    if (rename(pkg,held)) {
        printf("[dlc-pilot] native PKG rename failed errno=%d\n",errno);goto out;
    }
    printf("[dlc-pilot] HELD original SSD PKG as ac.pkg" PILOT_HELD_SUFFIX " (no deletion)\n");fflush(stdout);
    if (nullfs_mount(r->source,r->native)) {
        printf("[dlc-pilot] overlay mount failed errno=%d\n",errno);goto out;
    }
    ssize_t n=readlink(pkg,actual,sizeof(actual)-1);
    if (n>0) actual[n]=0;
    if (n<=0 || strcmp(actual,r->usb) || top_layer(r->native,r->source)!=1 ||
        owned_layer_count(r->native,r->source)!=1) {
        printf("[dlc-pilot] overlay validation failed\n");goto out;
    }
    ok=1;
    __atomic_store_n(&g_pilot_stage,3,__ATOMIC_RELEASE);
    printf("[dlc-pilot] ACTIVE ONE %s: ac.pkg=%s; SSD original safely HELD, DB unchanged\n",cid,actual);
out:
    if (!ok) {
        printf("[dlc-pilot] FAILED %s; attempting rollback\n",cid);
        if (journal && pilot_restore_internal(1))
            printf("[dlc-pilot] MANUAL RECOVERY REQUIRED; do not disconnect USB\n");
        __atomic_store_n(&g_pilot_stage,4,__ATOMIC_RELEASE);
    }
    if (opened) src_close(t);
    free(t);free(r);
    printf("[dlc-pilot] WORKER EXIT %s\n",ok?"ACTIVE":"FAILED");fflush(stdout);
    __atomic_store_n(&g_pilot_job,0,__ATOMIC_RELEASE);
    return NULL;
}
int app_pilot_redirect(const char *tid,const char *cid,char *msg,size_t cap) {
    if (strcmp(tid,PILOT_TID) || !pilot_cid_valid(cid)) {
        snprintf(msg,cap,"Teste limitado a 1 DLC de " PILOT_TID);return -1;
    }
    if (redirect_locked() || redirect_busy() || pilot_title_running()) {
        snprintf(msg,cap,"Jogo ocupado, outra operacao ativa ou DLC aguardando restauracao");return -1;
    }
    if (!db_snapshot_healthy() || !db_is_registered(tid)) {
        snprintf(msg,cap,"Jogo base nao registrado ou DB indisponivel");return -1;
    }
    const char *ids[]={cid};int states[]={-1};
    db_dlc_registration(ids,1,states);
    if (states[0]!=1) {
        snprintf(msg,cap,"DLC ainda nao registrado. Primeiro instale somente ele nativamente.");return -1;
    }
    snprintf(g_pilot_cid,sizeof(g_pilot_cid),"%s",cid);
    __atomic_store_n(&g_pilot_job,1,__ATOMIC_RELEASE);
    __atomic_store_n(&g_pilot_stage,1,__ATOMIC_RELEASE);
    pthread_attr_t attr;int rc=pthread_attr_init(&attr);
    if (!rc) {
        rc=pthread_attr_setstacksize(&attr,512*1024);
        if (rc) pthread_attr_destroy(&attr);
    }
    if (rc) {
        __atomic_store_n(&g_pilot_job,0,__ATOMIC_RELEASE);
        snprintf(msg,cap,"Falha na preparacao da thread %d",rc);return -1;
    }
    pthread_t worker;rc=pthread_create(&worker,&attr,pilot_worker,NULL);pthread_attr_destroy(&attr);
    if (rc) {
        __atomic_store_n(&g_pilot_job,0,__ATOMIC_RELEASE);
        snprintf(msg,cap,"Falha ao iniciar thread %d",rc);return -1;
    }
    pthread_detach(worker);
    printf("[dlc-pilot] QUEUED %s (one DLC, temporary SSD rename, USB nullfs)\n",cid);
    snprintf(msg,cap,"Piloto de 1 DLC iniciado; acompanhe [dlc-pilot]");return 0;
}

/* Populate choices from actual USB content IDs; never trust an arbitrary
 * client-supplied ID when installing or mounting. */
int app_pilot_options(const char *tid,buf_t *out) {
    if (strcmp(tid,PILOT_TID)) return -1;
    char root[32];title_src_t *t=calloc(1,sizeof(*t));
    if (!t) return -1;
    if (find_root(tid,root,sizeof(root)) || !is_mountpoint(root) || src_open(t,root,tid)) {
        free(t);return -1;
    }
    buf_printf(out,"{\"dlcs\":[");
    const char *ids[MAX_DLC];int states[MAX_DLC];
    for (int i=0;i<t->ndlc;i++) ids[i]=t->dlc[i].pkg.content_id;
    if (t->ndlc) db_dlc_registration(ids,t->ndlc,states);
    for (int i=0;i<t->ndlc;i++) {
        if (i) buf_printf(out,",");
        buf_printf(out,"{\"content_id\":");json_str(out,ids[i]);
        buf_printf(out,",\"registered\":%s}",states[i]==1?"true":"false");
    }
    buf_printf(out,"]}");src_close(t);free(t);return 0;
}

void app_progress_json(buf_t *out) {
    buf_printf(out, "{\"phase\":"); json_str(out, g_progress.phase);
    buf_printf(out, ",\"total\":%d,\"done\":%d,\"ok\":%d,\"failed\":%d,\"current\":",
               g_progress.total, g_progress.done, g_progress.ok, g_progress.failed);
    json_str(out, g_progress.current);
    buf_printf(out, ",\"detail\":"); json_str(out, g_progress.detail);
    buf_printf(out, ",\"scan_folders\":%d,\"scan_pkgs\":%d,\"scan_organized\":%d,\"scan_path\":",
               g_scan_folders, g_scan_examined, g_scan_organized);
    json_str(out, g_scan_location);
    buf_printf(out, ",\"dlc_percent\":%d,\"dlc_content\":", g_dlc_percent);
    json_str(out, g_dlc_content);
    buf_printf(out, ",\"redirect_running\":%d,\"redirect_stage\":%d,\"redirect_done\":%d,\"redirect_current\":%d,\"redirect_total\":%d",
               __atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE),
               __atomic_load_n(&g_redirect_stage, __ATOMIC_ACQUIRE),
               __atomic_load_n(&g_redirect_done, __ATOMIC_ACQUIRE),
               __atomic_load_n(&g_redirect_current, __ATOMIC_ACQUIRE), REDIRECT_COUNT);
    buf_printf(out, ",\"pilot_running\":%d,\"pilot_stage\":%d,\"pilot_active\":%d",
               __atomic_load_n(&g_pilot_job,__ATOMIC_ACQUIRE),
               __atomic_load_n(&g_pilot_stage,__ATOMIC_ACQUIRE),exists(PILOT_JOURNAL));
    buf_printf(out, "}");
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
            c.dlc = 1; /* legacy setting ignored: all DLCs always included */
        else if (!strncmp(line, "path=", 5) && c.npaths < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 5, path, sizeof(path), why, sizeof(why)))
            snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
    }
    fclose(f);
    if (c.batch_size < 1 || c.batch_size > 50)
        c.batch_size = 5;
    if (!c.npaths)
        c.npaths = 1; /* the root */
    c.dlc = 1;
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
                c.dlc = 1; /* keep compatibility with older panel clients */
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
    c.dlc = 1;
    g_cfg = c;
    config_save();
    printf("[config] saved: batch=%d batch_size=%d dlc=%d folders=%d\n", c.batch, c.batch_size, c.dlc, c.npaths);
    snprintf(msg, cap, "Settings saved");
    return 0;
}

/* Panel remains available at http://<PS5 IP>:8770. Existing Media-tab
 * shortcut stays installed if present; NEVER run a global installer at boot. */

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
    g_appinst_ready = sceAppInstUtilInitialize() == 0;
    if (!g_appinst_ready) fprintf(stderr, "[dlc-install] AppInstUtil initialization unsuccessful\n");
    mkdirs(STATE_DIR, 0777);
    /* Keep stdout alive if opening log fails (upstream 0.5.0 regression fix). */
    int logfd = open(STATE_DIR "/log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (logfd >= 0) {
        if (dup2(logfd, STDOUT_FILENO) < 0)
            fprintf(stderr, "[main] log redirection failed: %s\n", strerror(errno));
        close(logfd);
    } else fprintf(stderr, "[main] cannot open log file: %s\n", strerror(errno));
    setvbuf(stdout, NULL, _IOLBF, 0);
    config_load();
    signal(SIGPIPE, SIG_IGN);
    /* Do not run AppInstallAll merely to create a panel shortcut. Use :8770. */
    g_web = web_listen();
    printf("[main] " VERSION " panel %s (http://<PS5 IP>:8770)\n", g_web >= 0 ? "ready" : "unavailable");
    unlink(STATE_DIR "/stop");
    if (exists(PILOT_JOURNAL)) {
        printf("[dlc-pilot] STARTUP recovery journal detected; restoring native DLC before normal restore\n");
        if (pilot_restore_internal(1))
            printf("[dlc-pilot] STARTUP RECOVERY FAILED: keep USB connected, use Restore pilot button\n");
    }
    unsigned last_usb_mask = 0;
    time_t last_usb_check = 0;
    int restore_on_reconnect = 0;
    int previously_active = restore_active_games();
    offload_resume_all();
    for (int i = 0; i < 10; ++i) {
        char root[32];
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (is_mountpoint(root)) last_usb_mask |= 1u << i;
    }
    last_usb_check = time(NULL);
    if (!previously_active)
        notify(g_web >= 0 ? VERSION " ready. Panel at http://<PS5 IP>:8770" : VERSION " panel unavailable");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            if (__atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE) ||
                __atomic_load_n(&g_pilot_job, __ATOMIC_ACQUIRE)) {
                pump_events(250); /* do not terminate while worker has mounted some DLCs */
                continue;
            }
            unlink(STATE_DIR "/stop");
            if (exists(PILOT_JOURNAL) && pilot_restore_internal(1)) {
                printf("[dlc-pilot] STOP refused: cannot restore pilot safely\n");
                return 0;
            }
            if (redirect_mounted() && !__atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE) && !title_running("CUSA03842")) {
                char restore_msg[256];
                app_restore_native_dlc("CUSA03842", restore_msg, sizeof(restore_msg));
                printf("[dlc-redirect] exit cleanup: %s\n", restore_msg);
            }
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
        if (!g_deleting && g_discover_requested) discover_usb();
        if (!g_deleting && g_activate_request[0]) activate_queued();
        if (!g_deleting && g_dlc_register_request[0]) register_dlcs_queued();
        if (!g_deleting && g_dlc_install_request[0]) install_dlcs_queued();
                pump_events(1000);
        /* Lightweight USB polling: no PKG walk. Restore only known, active titles
         * after a genuinely new storage mount appears. Never discover automatically. */
        time_t now = time(NULL);
        if (now - last_usb_check >= 10) {
            last_usb_check = now;
            unsigned mask = 0;
            for (int i = 0; i < 10; ++i) {
                char root[32];
                if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
                else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
                if (is_mountpoint(root)) mask |= 1u << i;
            }
            if (mask != last_usb_mask) {
                printf("[usb] mount set changed 0x%03x -> 0x%03x\n", last_usb_mask, mask);
                g_catalog_dirty = 1;
                if (mask & ~last_usb_mask) restore_on_reconnect = 1;
                last_usb_mask = mask;
            }
        }
        /* A removed USB makes the temporary ac.pkg link invalid: restore native
         * directory when the pilot title is not running. */
        if (redirect_mounted() && !__atomic_load_n(&g_redirect_job_active, __ATOMIC_ACQUIRE) &&
            !title_running("CUSA03842") && redirect_usb_missing()) {
            char restore_msg[256];
            app_restore_native_dlc("CUSA03842", restore_msg, sizeof(restore_msg));
            printf("[dlc-redirect] missing USB source auto-restore: %s\n", restore_msg);
        }
        if (exists(PILOT_JOURNAL) && !__atomic_load_n(&g_pilot_job,__ATOMIC_ACQUIRE)) {
            pilot_record_t pil;
            if (!pilot_read(&pil) && !pilot_title_running() && !exists(pil.usb)) {
                printf("[dlc-pilot] USB missing: restoring native PKG\n");
                pilot_restore_internal(1);
            }
        }
        if (restore_on_reconnect && !redirect_locked() && !g_deleting && !g_discover_requested &&
            !g_activate_request[0] && !g_dlc_register_request[0] && !g_restore_running &&
            strcmp(g_progress.phase, "searching") && strcmp(g_progress.phase, "activating") &&
            strcmp(g_progress.phase, "registering_dlcs") && strcmp(g_progress.phase, "dlc_register_queued")) {
            offload_resume_all();
            restore_on_reconnect = 0;
            printf("[usb] newly connected storage; restoring previously active titles only\n");
            restore_active_games();
        }
    }
}
