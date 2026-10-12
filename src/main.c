/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* Safety-preview design:
 * - Default discovery only READS USB PKGs, including nested updates.
 * - Only explicit /api/mount requests can move/rename into USB/ps4/ and
 *   register one title; no direct SQLite fallback.
 * - DLCs (ac) are ignored; existing games and system DB are never bulk-updated.
 * - Any unconfirmed registration blocks further requests until restart.
 * WARNING: experimental homebrew; test on expendable data, NOT a production PS5. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#include "db.h"
#include "pkg.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.4.0-safe-preview"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
#define STATE_DIR "/data/ps4mount"
#define MAX_TITLES 256
#define MAX_FOLDER_DEPTH 8
/* USB discovery only runs when USB presence changes or rescan is requested. */
#define MAX_SAFE_NAME 72
/* Until isolated console tests are completed, forbid ALL manual registrations.
 * Flip only in a dedicated test branch after backup/rollback planning. */
#define ENABLE_EXPERIMENTAL_REGISTRATION 0

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);
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

static int64_t file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int64_t)st.st_mtime;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

static int is_mountpoint(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) == 0 && !strcmp(sf.f_mntonname, path);
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

/* One library folder: <root>/ps4/<TID>/app.pkg plus an optional patch.pkg. */
typedef struct {
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    pkg_t app, patch;
    int has_patch;
    sfo_t app_sfo, patch_sfo;
    char name[128], ver[8], marker[PATH_MAX + 128];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
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
}

static int first_time(const char *path);

static int src_open(title_src_t *t, const char *dir, const char *tid) {
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "%s", dir);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    if (pkg_open(&t->app, t->app_path) || read_sfo(&t->app, &t->app_sfo)) {
        printf("[scan] %s: unreadable app pkg\n", t->app_path);
        if (t->app.fd >= 0)
            pkg_close(&t->app);
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

static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id, *cid = app->content_id;
    const sfo_item_t *av = sfo_get(&t->app_sfo, "APP_VER");
    const char *app_ver = av && !av->is_int ? av->sval : "01.00";
    char p[PATH_MAX], q[PATH_MAX];
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

    size_t lic_len;
    uint8_t *lic = pkg_entry(app, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        free(lic);
        printf("[reg] %s: no license.dat\n", tid);
        return -1;
    }
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, app, lic);
    free(lic);
    snprintf(q, sizeof(q), "/user/license/fake%.19s.rif", cid);
    rc |= write_buf(q, &rif);
    snprintf(q, sizeof(q), "/user/license/fake%.19s.idx", cid);
    rc |= write_buf(q, &idx);
    buf_free(&rif);
    buf_free(&idx);

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0}, pdb = {0}, patch_pdb = {0};
    build_app_json(&json, app, t->app_path);
    build_app_pbm(&pbm, app, app_ver, 0);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, t->patch_path);
        build_app_pbm(&ppbm_patch, pat, t->ver, 1);
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
    snprintf(q, sizeof(q), "%s/app.json", t->dir);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", t->dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", t->dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", t->dir);
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
        snprintf(q, sizeof(q), "%s/patch.json", t->dir);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", t->dir);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", t->dir);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", t->dir);
        rc |= write_buf(q, &pxml);
        snprintf(p, sizeof(p), "/user/playgo/patch/%s/00/00.00", cid);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/d0.pdb", p);
        rc |= write_buf(q, &patch_pdb);
        snprintf(q, sizeof(q), "%s/d1.pdb", p);
        rc |= write_buf(q, &patch_pdb);
    } else {
        rm_tree(p);
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

/* Titles this daemon serves. The USB folder stays read-only (a system
 * uninstall then cannot touch it) except while the game is starting/running. */
typedef struct {
    int used, has_patch, rw, running;
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    time_t rw_until; /* selected on the home screen: back to read-only after this */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
static int g_deleting; /* a delete dialog is open: USB folders are unmounted */
static time_t g_deleting_since;

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our read-only layer, 2 our read-write layer, -1 something else. */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    if (strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir))
        return -1;
    return sf.f_flags & MNT_RDONLY ? 1 : 2;
}

static int nullfs_mount(const char *dir, const char *target, int rw) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    if (nmount(iov, 6, rw ? 0 : MNT_RDONLY)) {
        printf("[mount] %s -> %s (%s): %s\n", dir, target, rw ? "rw" : "ro", strerror(errno));
        return -1;
    }
    return 0;
}

/* One layer per path: switching read-only <-> read-write is unmount + mount
 * (this nullfs supports neither MNT_UPDATE nor a writable layer on top). */
/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
static int title_running(const char *tid) {
    char want[64];
    snprintf(want, sizeof(want), "/mnt/sandbox/pfsmnt/%s-app0", tid);
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < n && !found; i++)
        found = !strcmp(list[i].f_mntonname, want);
    free(list);
    return found;
}

static int mount_dir(const char *dir, const char *target, int rw) {
    mkdir(target, 0777);
    int top = top_layer(target, dir);
    if (top == (rw ? 2 : 1))
        return 0;
    while (top != 0) {
        if (unmount(target, 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        top = top_layer(target, dir);
    }
    return nullfs_mount(dir, target, rw);
}

static void unmount_all_layers(const char *target, int force) {
    for (int i = 0; i < 8 && mounted_from(target, NULL, 0); i++)
        if (unmount(target, force ? MNT_FORCE : 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            if (!force)
                return;
        }
}

/* Removes every layer of the title (forced: a system uninstall is about to run). */
static void unmount_title(const char *tid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    unmount_all_layers(target, 0);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    unmount_all_layers(target, 0);
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update. */
static int mount_paths(const char *dir, const char *tid, int has_patch, int rw) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    if (mount_dir(dir, target, rw))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    if (has_patch)
        return mount_dir(dir, target, rw);
    unmount_all_layers(target, 0);
    return 0;
}

static int mount_title(const title_src_t *t, int rw) { return mount_paths(t->dir, t->app.title_id, t->has_patch, rw); }

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
        m->rw = keep.rw;
        m->running = keep.running;
        m->running_since = keep.running_since;
        m->rw_until = keep.rw_until;
    }
    m->has_patch = t->has_patch;
    snprintf(m->tid, sizeof(m->tid), "%s", t->app.title_id);
    snprintf(m->cid, sizeof(m->cid), "%s", t->app.content_id);
    snprintf(m->name, sizeof(m->name), "%s", t->name);
    snprintf(m->dir, sizeof(m->dir), "%s", t->dir);
}

static int top_layer_rw(const managed_t *m) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", m->tid);
    return top_layer(target, m->dir) == 2;
}

/* Desired state is kept even when the switch fails; the next scan retries it.
 * Going writable retries for ~300 ms: the system may hold a file for a moment. */
static void set_rw(managed_t *m, int rw) {
    m->rw = rw;
    int rc = mount_paths(m->dir, m->tid, m->has_patch, rw);
    for (int i = 0; rc && rw && i < 15; i++) {
        usleep(20000);
        rc = mount_paths(m->dir, m->tid, m->has_patch, rw);
    }
    printf("[mount] %s %s%s\n", m->tid, rw ? "read-write" : "read-only", rc ? " (pending)" : "");
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
    char path[64], buf[PATH_MAX + 128] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strcmp(buf, expected);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
static int title_state(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (installed_internally(tid) || removed(tid))
        return -1;
    char pdb[PATH_MAX];
    snprintf(pdb, sizeof(pdb), "/user/playgo/app/%s/00/d0.pdb", t->app.content_id);
    int registered = db_registered_status(tid);
    if (registered < 0) return -2; /* cannot treat DB failure as missing */
    return !registered || !exists(pdb) || !marker_matches(tid, t->marker);
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

static int g_registration_busy;
static int g_registration_fault;
static char g_queued_tid[10];

static int register_title(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (g_registration_busy) return -1;
    g_registration_busy = 1;
    int result = -1;
    int was_registered = db_registered_status(tid);
    if (was_registered < 0) {
        printf("[safe][db] %s cannot read registration state; ABORT\n", tid);
        goto done;
    }
    printf("[safe][reg] %s BEGIN, registered=%d\n", tid, was_registered);
    if (place_files(t) || mount_title(t, 1)) {
        printf("[safe][reg] %s preparation/mount FAILED; no new registration\n", tid);
        goto done;
    }
    manage(t);
    if (!was_registered) {
        int rc = sceAppInstUtilAppInstallAll(NULL);
        printf("[safe][reg] %s AppInstallAll=0x%08x\n", tid, rc);
        if (rc) goto done;
        /* Wait for confirmation; do NOT write directly into SQLite on timeout. */
        int status = 0;
        for (int i = 0; i < 15; ++i) {
            status = db_registered_status(tid);
            if (status != 0) break;
            /* pump_events serves the web panel; mutating endpoints are guarded. */
            pump_events(1000);
        }
        if (status != 1) {
            printf("[safe][reg] %s NOT CONFIRMED (db_status=%d), no SQLite fallback\n", tid, status);
            goto done;
        }
    }
    /* The shell registered the title, or it was already registered. */
    int status = db_registered_status(tid);
    if (status != 1) {
        printf("[safe][reg] %s DB verification FAILED (%d)\n", tid, status);
        goto done;
    }
    enforce_patch_sfo(t);
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666)) goto done;
    manage(t);
    managed_t *m = managed_by(tid, NULL);
    if (mount_title(t, m && m->rw)) goto done;
    printf("[safe][reg] %s CONFIRMED\n", tid);
    result = 0;
done:
    if (result) {
        g_registration_fault = 1;
        printf("[safe][reg] %s FAILED; further registration BLOCKED until restart\n", tid);
    }
    g_registration_busy = 0;
    return result;
}

/* Logs a skipped root file once per daemon run. */
static int first_time(const char *path) {
    static char seen[64][PATH_MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(seen[i], path))
            return 0;
    if (n < 64)
        snprintf(seen[n++], PATH_MAX, "%s", path);
    return 1;
}

/* Discovery catalog: all paths remain UNTOUCHED until a user explicitly adds
 * one selected title. The index joins base and update by metadata CUSA across
 * different folders. More than one distinct base or update is AMBIGUOUS. */
typedef struct {
    char tid[10];
    char root[32];
    char dir[PATH_MAX];       /* before preparation: the base file's directory */
    char app_path[PATH_MAX];  /* original, possibly nested, PKG path */
    char patch_path[PATH_MAX];
    char name[128], ver[16];
    unsigned long long size, patch_size;
    int conflict, patch_conflict;
    int state;
} title_t;

static title_t g_titles[MAX_TITLES];
static int g_title_count;
static int g_manual_scan;
static int g_last_seen[10];

static title_t *catalog_find(const char *tid) {
    for (int i = 0; i < g_title_count; i++)
        if (!strcmp(g_titles[i].tid, tid)) return &g_titles[i];
    return NULL;
}

static void catalog_forget_root(const char *root) {
    for (int i = 0; i < g_title_count;)
        if (!strcmp(g_titles[i].root, root)) {
            memmove(g_titles + i, g_titles + i + 1,
                    (size_t)(g_title_count - i - 1) * sizeof(g_titles[0]));
            g_title_count--;
        } else i++;
}

/* Read-only metadata extraction. Retail and DLC (ac) are ignored. */
static int describe_pkg(const char *path, char tid[10], int *category,
                        char name[128], char ver[16], unsigned long long *size) {
    pkg_t pkg;
    if (pkg_open(&pkg, path)) return -1;
    sfo_t sfo;
    int ok = !read_sfo(&pkg, &sfo);
    const sfo_item_t *cat = ok ? sfo_get(&sfo, "CATEGORY") : NULL;
    int kind = cat && !cat->is_int && !strcmp(cat->sval, "gd") ? 1 :
               cat && !cat->is_int && !strcmp(cat->sval, "gp") ? 2 : 0;
    ok = kind && strlen(pkg.title_id) == 9 && !strncmp(pkg.title_id, "CUSA", 4) && pkg_fake_key(&pkg);
    if (ok) {
        const sfo_item_t *n = sfo_get(&sfo, "TITLE"), *v = sfo_get(&sfo, "APP_VER");
        snprintf(tid, 10, "%s", pkg.title_id);
        *category = kind;
        snprintf(name, 128, "%s", n && !n->is_int ? n->sval : pkg.title_id);
        snprintf(ver, 16, "%s", v && !v->is_int ? v->sval : "01.00");
        *size = (unsigned long long)pkg.pkg_size;
    }
    pkg_close(&pkg);
    return ok ? 0 : -1;
}

static void inspect_pkg(const char *root, const char *path) {
    char tid[10], name[128], ver[16];
    unsigned long long size = 0;
    int kind;
    if (describe_pkg(path, tid, &kind, name, ver, &size)) return;
    title_t *t = catalog_find(tid);
    if (!t) {
        if (g_title_count >= MAX_TITLES) {
            printf("[safe][scan] MAX_TITLES reached; ignoring %s\n", path);
            return;
        }
        t = &g_titles[g_title_count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->tid, sizeof(t->tid), "%s", tid);
        snprintf(t->root, sizeof(t->root), "%s", root);
    }
    if (strcmp(t->root, root)) {
        /* One CUSA from two connected drives cannot be selected safely. */
        t->conflict = 1;
        printf("[safe][scan] %s found on multiple drives; ambiguous\n", tid);
        return;
    }
    if (kind == 1) {
        if (t->app_path[0] && strcmp(t->app_path, path)) {
            t->conflict = 1;
            printf("[safe][scan] %s duplicate game base: %s\n", tid, path);
            return;
        }
        snprintf(t->app_path, sizeof(t->app_path), "%s", path);
        snprintf(t->name, sizeof(t->name), "%s", name);
        snprintf(t->ver, sizeof(t->ver), "%s", ver);
        t->size = size;
        snprintf(t->dir, sizeof(t->dir), "%s", path);
        char *slash = strrchr(t->dir, '/');
        if (slash) *slash = 0;
    } else {
        if (t->patch_path[0] && strcmp(t->patch_path, path)) {
            t->patch_conflict = 1;
            printf("[safe][scan] %s multiple updates; update will not be moved\n", tid);
            return;
        }
        snprintf(t->patch_path, sizeof(t->patch_path), "%s", path);
        t->patch_size = size;
    }
}

/* Read only, no package renames, no DB writes. Does not traverse symlinks.
 * Canonical /ps4 folders are scanned too, including CUSA - Name. */
static void scan_nested(const char *root, const char *dir, int depth) {
    if (depth > MAX_FOLDER_DEPTH) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, "System Volume Information") ||
            !strcmp(e->d_name, "$RECYCLE.BIN") || e->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (lstat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_nested(root, path, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            size_t n = strlen(e->d_name);
            if (n >= 5 && !strcasecmp(e->d_name + n - 4, ".pkg")) inspect_pkg(root, path);
        }
    }
    closedir(d);
}

/* Sanitizes TITLE for exFAT/Windows and keeps CUSA as stable folder prefix. */
static void safe_folder_name(const title_t *t, char out[128]) {
    char title[MAX_SAFE_NAME + 1];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)t->name; *p && j < MAX_SAFE_NAME; p++) {
        unsigned char c = *p;
        if (c < 32 || strchr("/\\:*?\"<>|", c)) c = '_';
        title[j++] = c;
    }
    while (j && (title[j - 1] == ' ' || title[j - 1] == '.')) j--;
    title[j] = 0;
    snprintf(out, 128, "%s - %s", t->tid, j ? title : "Game");
}

/* Same-volume rename only. Never overwrite; do not auto-replace an update.
 * A partial move can be discovered again by CUSA on the next manual scan. */
static int prepare_title(title_t *t) {
    if (!t || !t->app_path[0] || t->conflict || !is_mountpoint(t->root)) return -1;
    if (t->patch_conflict) {
        printf("[safe][prepare] %s multiple updates, refusing to choose one\n", t->tid);
        return -1;
    }
    if (installed_internally(t->tid)) {
        printf("[safe][prepare] %s installed internally; refusing to move\n", t->tid);
        return -1;
    }
    /* Keep older canonical /ps4/CUSAxxxxx/app.pkg layouts working without moves. */
    char lib[PATH_MAX], prefix[PATH_MAX], dest[PATH_MAX], app_dst[PATH_MAX], patch_dst[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", t->root);
    snprintf(prefix, sizeof(prefix), "%s/%s", lib, t->tid);
    size_t prefix_len = strlen(prefix);
    int old_canonical = !strncmp(t->app_path, prefix, prefix_len) &&
                        t->app_path[prefix_len] == '/' && !strcmp(t->app_path + prefix_len + 1, "app.pkg");
    if (old_canonical) snprintf(dest, sizeof(dest), "%s", prefix);
    else {
        char folder[128]; safe_folder_name(t, folder);
        if (snprintf(dest, sizeof(dest), "%s/%s", lib, folder) >= (int)sizeof(dest)) return -1;
    }
    if (snprintf(app_dst, sizeof(app_dst), "%s/app.pkg", dest) >= (int)sizeof(app_dst) ||
        snprintf(patch_dst, sizeof(patch_dst), "%s/patch.pkg", dest) >= (int)sizeof(patch_dst)) return -1;
    if ((strcmp(t->app_path, app_dst) && exists(app_dst)) ||
        (t->patch_path[0] && strcmp(t->patch_path, patch_dst) && exists(patch_dst))) {
        printf("[safe][prepare] %s destination conflict; original files left intact\n", t->tid);
        return -1;
    }
    struct stat st;
    if ((stat(lib, &st) && mkdir(lib, 0777)) ||
        (stat(dest, &st) && mkdir(dest, 0777))) {
        printf("[safe][prepare] %s mkdir failed: %s\n", t->tid, strerror(errno));
        return -1;
    }
    /* Verify metadata again before changing any paths. */
    char id[10], name[128], ver[16]; int kind; unsigned long long bytes;
    if (describe_pkg(t->app_path, id, &kind, name, ver, &bytes) || kind != 1 || strcmp(id, t->tid)) return -1;
    if (t->patch_path[0] &&
        (describe_pkg(t->patch_path, id, &kind, name, ver, &bytes) || kind != 2 || strcmp(id, t->tid))) return -1;
    char original[PATH_MAX]; snprintf(original, sizeof(original), "%s", t->app_path);
    int moved_app = 0;
    if (strcmp(t->app_path, app_dst)) {
        if (rename(t->app_path, app_dst)) {
            printf("[safe][prepare] %s base rename FAILED: %s\n", t->tid, strerror(errno));
            return -1;
        }
        moved_app = 1;
        printf("[safe][prepare] base %s -> %s\n", original, app_dst);
    }
    if (t->patch_path[0] && strcmp(t->patch_path, patch_dst) && rename(t->patch_path, patch_dst)) {
        printf("[safe][prepare] %s patch rename FAILED: %s\n", t->tid, strerror(errno));
        if (moved_app && rename(app_dst, original))
            printf("[safe][prepare] CRITICAL rollback failed; base now %s\n", app_dst);
        return -1;
    }
    if (t->patch_path[0]) printf("[safe][prepare] patch -> %s\n", patch_dst);
    snprintf(t->dir, sizeof(t->dir), "%s", dest);
    snprintf(t->app_path, sizeof(t->app_path), "%s", app_dst);
    if (t->patch_path[0]) snprintf(t->patch_path, sizeof(t->patch_path), "%s", patch_dst);
    printf("[safe][prepare] %s ready for explicit registration\n", t->tid);
    return 0;
}

static void scan_root(const char *root) {
    catalog_forget_root(root);
    scan_nested(root, root, 0);
    int games = 0, conflicts = 0, updates = 0;
    for (int i = 0; i < g_title_count; i++) {
        title_t *t = &g_titles[i];
        if (strcmp(root, t->root) || !t->app_path[0]) continue;
        games++;
        if (t->conflict || t->patch_conflict) conflicts++;
        if (t->patch_path[0]) updates++;
    }
    printf("[safe][scan] %s: games=%d with_updates=%d conflicts=%d; NO files moved, NO games registered\n",
           root, games, updates, conflicts);
    notify("%d games indexed on %s (read-only); use web panel to add one", games, root);
}

static void scan_all(void) {
    char root[32];
    for (int i = 0; i < 10; i++) {
        if (i < 8) snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        int present = is_mountpoint(root);
        if (present && (!g_last_seen[i] || g_manual_scan)) scan_root(root);
        else if (!present && g_last_seen[i]) {
            printf("[safe][scan] disconnected %s, forgetting catalog without force-unmount\n", root);
            catalog_forget_root(root);
        }
        g_last_seen[i] = present;
    }
    g_manual_scan = 0;
}

/* Remove stale mounts only through explicit recovery tooling (not on a timer).
 * Forced unmount during game activity is unsafe. */

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
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            set_rw(&g_managed[i], g_managed[i].rw);
}

/* Copies klog lines about our titles (and crash/mount errors) into the log. */
static void trace_klog(const char *line) {
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", NULL};
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
    /* Do not allow reentrant mount/unmount and deletion handling while a
     * register operation has yielded to pump_events(). */
    if (g_registration_busy) return;
    if ((p = after(line, "NextImageUri=/user/appmeta/"))) {
        /* game selected on the home screen: get writable before the launch */
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m && !g_registration_busy && !g_deleting && !m->running) {
            if (!m->rw)
                set_rw(m, 1);
            m->rw_until = time(NULL) + 90;
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m && !g_registration_busy && !g_deleting) {
            m->running = 1;
            m->running_since = time(NULL);
            m->rw_until = 0;
            if (!m->rw || top_layer_rw(m) != 1)
                set_rw(m, 1);
        }
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            m->rw_until = 0;
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid);
                printf("[delete] %s closed, USB folder unmounted\n", m->tid);
            } else {
                set_rw(m, 0);
            }
        }
    } else if (strstr(line, "-> [AppDeleteConfirmScene")) {
        /* Delete dialog: take the USB folders away so the uninstall only removes the entry. */
        g_deleting = 1;
        g_deleting_since = time(NULL);
        for (int i = 0; i < MAX_TITLES; i++) {
            managed_t *m = &g_managed[i];
            if (!m->used)
                continue;
            if (m->running || title_running(m->tid))
                continue; /* still open: unmounted when it closes (see the End event) */
            unmount_title(m->tid);
        }
        printf("[delete] dialog open, USB folders unmounted\n");
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m) {
            char path[64];
            snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
            write_file(path, m->dir, strlen(m->dir), 0666);
            snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
            unlink(path);
            notify("%s removed from the library. The pkg stays on the USB drive.", m->name);
            m->used = 0;
        }
    } else if (g_deleting && (strstr(line, "[AppDeletingScene : AppDeletingScene] -> [") ||
                              (strstr(line, "[AppDeleteConfirmScene : AppDeleteConfirmScene] -> [") &&
                               !strstr(line, "AppDeletingScene]")))) {
        printf("[delete] dialog closed\n");
        end_delete_window();
    }
}

/* --- web panel --- */

static time_t g_next_scan;
static int g_stop;

void app_rescan(void) {
    if (!g_registration_busy) { g_next_scan = 0; g_manual_scan = 1; }
}
void app_stop(void) { g_stop = 1; }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20) buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"busy\":%s,\"fault\":%s,\"games\":[",
               (g_registration_busy || g_queued_tid[0]) ? "true" : "false",
               g_registration_fault ? "true" : "false");
    int first = 1;
    for (int i = 0; i < g_title_count; i++) {
        title_t *t = &g_titles[i];
        if (!t->app_path[0]) continue; /* orphan update, no base game */
        const char *state = "discovered";
        managed_t *m = managed_by(t->tid, NULL);
        if (t->conflict || t->patch_conflict) state = "conflict";
        else if (g_queued_tid[0] && !strcmp(g_queued_tid, t->tid)) state = "queued";
        else if (installed_internally(t->tid)) state = "internal";
        else if (m && m->running) state = "running";
        else if (m) state = "ready";
        buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", t->tid);
        json_str(out, t->name);
        buf_printf(out, ",\"version\":"); json_str(out, t->ver);
        buf_printf(out, ",\"size\":%llu,\"has_patch\":%s,\"state\":\"%s\",\"mount\":\"\",\"source\":",
                   t->size + t->patch_size, t->patch_path[0] ? "true" : "false", state);
        json_str(out, t->app_path);
        buf_printf(out, ",\"update_issue\":");
        json_str(out, t->conflict ? "Duplicate base PKGs for this CUSA; no action allowed" :
                       t->patch_conflict ? "Multiple updates for this CUSA; no action allowed" : "");
        buf_printf(out, "}");
        first = 0;
    }
    buf_printf(out, "]}");
}

int app_icon(const char *tid, buf_t *out) {
    title_t *t = catalog_find(tid);
    if (!t || !t->app_path[0]) return -1;
    /* Prefer reading artwork from the USB PKG; no writes to internal appmeta. */
    pkg_t pkg;
    if (pkg_open(&pkg, t->app_path)) return -1;
    size_t len;
    uint8_t *png = pkg_entry(&pkg, 0x1200, &len);
    pkg_close(&pkg);
    if (!png) return -1;
    buf_put(out, png, len);
    free(png);
    return 0;
}

int app_add(const char *tid, char *msg, size_t cap) {
    title_t *t = catalog_find(tid);
#if !ENABLE_EXPERIMENTAL_REGISTRATION
    (void)t;
    snprintf(msg, cap, "Safety preview: registration disabled until console validation");
    return -1;
#endif
    if (!t || !t->app_path[0]) {
        snprintf(msg, cap, "%s is not indexed (scan first)", tid); return -1;
    }
    if (t->conflict || t->patch_conflict) {
        snprintf(msg, cap, "Ambiguous game/update PKGs; fix duplicates before adding"); return -1;
    }
    if (g_registration_busy || g_queued_tid[0] || g_registration_fault) {
        snprintf(msg, cap, "Registration busy or blocked after a failure; check log"); return -1;
    }
    if (installed_internally(tid)) {
        snprintf(msg, cap, "%s is installed internally; refusing to modify", tid); return -1;
    }
    snprintf(g_queued_tid, sizeof(g_queued_tid), "%s", tid);
    snprintf(msg, cap, "Queued ONE game (%s); see log for confirmation", tid);
    printf("[safe][queue] %s queued by user\n", tid);
    return 0;
}

int app_remove(const char *tid, char *msg, size_t cap) {
    (void)tid;
    /* Disabled in preview: uninstall can touch system DB and trigger forced
     * unmounts. Never expose a mutation we have not tested in isolation. */
    snprintf(msg, cap, "Removal disabled in safe preview; no data changed");
    return -1;
}

static void process_queue(void) {
    if (!ENABLE_EXPERIMENTAL_REGISTRATION || !g_queued_tid[0] || g_registration_busy || g_registration_fault) return;
    char tid[10]; snprintf(tid, sizeof(tid), "%s", g_queued_tid);
    g_queued_tid[0] = 0;
    title_t *t = catalog_find(tid);
    if (!t || !t->app_path[0] || t->conflict || t->patch_conflict) {
        printf("[safe][queue] %s invalid / conflict; operation aborted\n", tid);
        g_registration_fault = 1; return;
    }
    if (prepare_title(t)) {
        printf("[safe][queue] %s prepare FAILED; blocking further requests\n", tid);
        g_registration_fault = 1; return;
    }
    title_src_t src;
    if (src_open(&src, t->dir, t->tid)) {
        printf("[safe][queue] %s package reopened FAILED\n", tid);
        g_registration_fault = 1; return;
    }
    int rc = register_title(&src);
    src_close(&src);
    if (rc) notify("%s registration FAILED; all further registrations blocked", tid);
    else notify("%s registered and verified. No bulk registration attempted", tid);
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
    rc = sceAppInstUtilAppInstallAll(NULL);
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
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a))) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
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
    if (single_instance() < 0) {
        printf(VERSION " already running\n");
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    mkdirs(STATE_DIR, 0777);
    freopen(STATE_DIR "/log.txt", "a", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    /* SAFE PREVIEW: shortcut installation invokes AppInstallAll and writes
     * to PS5 DB even during discovery; never install it automatically. */
    printf("[safe] shortcut install DISABLED; open http://<PS5 IP>:8770\n");
    g_web = web_listen();
    notify(g_web >= 0 ? VERSION " running. Read-only scan; panel http://<PS5 IP>:8770" : VERSION " panel unavailable");
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            notify(VERSION " stopped");
            return 0;
        }
        if (g_deleting && time(NULL) - g_deleting_since > 120)
            end_delete_window(); /* dialog close line missed */
        for (int i = 0; i < MAX_TITLES; i++) {
            managed_t *m = &g_managed[i];
            if (m->used && m->rw && !m->running && m->rw_until && time(NULL) > m->rw_until) {
                m->rw_until = 0;
                set_rw(m, 0);
            }
        }
        if (time(NULL) >= g_next_scan && !g_deleting && !g_registration_busy) {
            scan_all();
            g_next_scan = time(NULL) + SCAN_SECONDS;
        }
        process_queue();
        pump_events(1000);
    }
}
