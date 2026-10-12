/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* Safety-preview design:
 * - Discovery only READS USB PKGs; explicit per-title USB relocation requires
 *   a fresh preview nonce and journal, NEVER automatic registration.
 * - Registration and preparation are disabled from the web/API in this build.
 * - Historical code paths for explicit preparation remain for isolated tests.
 * - No direct SQLite fallback.
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
#include <strings.h>
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
#include "update_version.h"
#include "web.h"
#include "organize.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.5.3-phase4b-mount-probe"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
#define STATE_DIR "/data/ps4mount"
#define MAX_TITLES 256
#define MAX_FOLDER_DEPTH 8
/* USB discovery only runs when USB presence changes or rescan is requested. */
#define MAX_SAFE_NAME 72
#define MAX_UPDATE_CANDIDATES 16
/* Never scan entire multi-GB updates just to remove a duplicate warning.
 * Exact byte comparison is only automatic for small files; large same-version
 * files remain blocked unless a different sample disproves equality. */
#define EXACT_COMPARE_MAX_BYTES (64ULL * 1024 * 1024)
#define COMPARE_BLOCK_BYTES (16 * 1024)
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

/* Atomic replacement for small metadata files. No O_TRUNC on the destination.
 * This does not make a sequence of metadata files a single transaction. */
static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    static unsigned int sequence;
    char tmp[PATH_MAX];
    struct stat st;
    if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        printf("[safe][io] refusing non-regular destination %s\n", path);
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.ps4mount-tmp-%ld-%u", path,
                 (long)getpid(), ++sequence) >= (int)sizeof(tmp)) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return -1;
    const unsigned char *p = data;
    size_t remaining = len;
    while (remaining) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        p += n;
        remaining -= (size_t)n;
    }
    int sync_rc = fsync(fd);
    int close_rc = close(fd);
    if (sync_rc || close_rc) {
        printf("[safe][io] sync/close failed %s\n", tmp);
        unlink(tmp);
        return -1;
    }
    /* Repeat the symlink guard immediately before replacing the destination. */
    if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
fail:
    close(fd);
    unlink(tmp);
    return -1;
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
            if (strcmp(t->patch.title_id, tid) || strcmp(t->patch.content_id, t->app.content_id)) {
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

/* Intentionally no recursive removal helper in this daemon. Uninstall, cleanup
 * and rollback of system metadata require a separately audited transaction. */

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
        /* Never recursively delete system playgo state during registration.
         * A stale patch must be reconciled in a separately audited workflow. */
        printf("[safe][reg] %s no patch: preserving any existing playgo files\n", tid);
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
    if (statfs(target, &sf)) return -1; /* unknown, never safe to unmount */
    if (strcmp(sf.f_mntonname, target)) return 0;
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
        return 1; /* fail closed when the running state cannot be determined */
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 1;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    int found = n < 0; /* fail closed on getfsstat error */
    for (int i = 0; i < n && !found; i++)
        found = !strcmp(list[i].f_mntonname, want);
    free(list);
    return found;
}

static int mount_dir(const char *dir, const char *target, int rw) {
    struct stat st;
    if (lstat(target, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return -1; /* no symlink targets */
    } else if (errno != ENOENT || mkdir(target, 0777)) {
        return -1;
    }
    int top = top_layer(target, dir);
    if (top == (rw ? 2 : 1)) return 0;
    if (top == -1) {
        printf("[safe][mount] %s occupied by foreign mount; refused\n", target);
        return -1;
    }
    const char *tid = strrchr(target, '/');
    if (!tid || title_running(tid + 1)) {
        printf("[safe][mount] %s running/unknown state; refusing transition\n", target);
        return -1;
    }
    if (top != 0) {
        if (unmount(target, 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        if (top_layer(target, dir) != 0) {
            printf("[safe][mount] unexpected layered mount at %s; refused\n", target);
            return -1;
        }
    }
    return nullfs_mount(dir, target, rw);
}

/* Only unmount our own layer, never force, and never while a game may run. */
static int unmount_owned_layer(const char *target, const char *dir) {
    int top = top_layer(target, dir);
    if (top == 0) return 0;
    if (top < 0) {
        printf("[safe][mount] foreign layer at %s; untouched\n", target);
        return -1;
    }
    const char *tid = strrchr(target, '/');
    if (!tid || title_running(tid + 1)) return -1;
    if (unmount(target, 0)) {
        printf("[safe][mount] unmount %s failed: %s\n", target, strerror(errno));
        return -1;
    }
    return 0;
}

static void unmount_title(const char *tid) {
    managed_t *m = managed_by(tid, NULL);
    if (!m) return;
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    unmount_owned_layer(target, m->dir);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    unmount_owned_layer(target, m->dir);
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
    return unmount_owned_layer(target, dir);
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
    char path[PATH_MAX];
    char content_id[37];
    char version[16];
    unsigned long long size;
    int suspicious_label;
} update_candidate_t;

typedef struct {
    char tid[10];
    char root[32];
    char dir[PATH_MAX];       /* before preparation: the base file's directory */
    char app_path[PATH_MAX];  /* original, possibly nested, PKG path */
    char patch_path[PATH_MAX];
    char name[128], ver[16], content_id[37], patch_version[16];
    unsigned long long size, patch_size;
    update_candidate_t patches[MAX_UPDATE_CANDIDATES];
    int update_count, update_overflow, invalid_updates, incompatible_updates;
    int suspicious_updates, verified_duplicate_updates, unverified_duplicate_updates;
    int conflict, patch_conflict;
    int state;
} title_t;

static title_t g_titles[MAX_TITLES];
static int g_title_count;
static int g_manual_scan;
static int g_last_seen[10];
static int g_scan_dlc_ignored;
static int g_scan_unreadable;

/* Filename/folder hints are NOT authoritative PKG metadata. Flag them for
 * human inspection rather than assuming CATEGORY=gp proves an update is safe.
 * Examples: /DLCs/Alien-DLC3.pkg or /Addons/patch.pkg. */
static int has_addon_label(const char *path) {
    for (const char *p = path; *p; ++p) {
        if (p != path && p[-1] != '/' && p[-1] != '-' && p[-1] != '_' &&
            p[-1] != '.' && p[-1] != ' ' && p[-1] != '+') continue;
        const char *word = NULL;
        size_t len = 0;
        if (!strncasecmp(p, "dlc", 3)) { word = p; len = 3; }
        else if (!strncasecmp(p, "addons", 6)) { word = p; len = 6; }
        else if (!strncasecmp(p, "add-on", 6)) { word = p; len = 6; }
        else if (!strncasecmp(p, "add_on", 6)) { word = p; len = 6; }
        if (!word) continue;
        unsigned char c = (unsigned char)p[len];
        if (!c || c == '/' || c == '.' || c == '-' || c == '_' ||
            c == ' ' || c == '+' || (c >= '0' && c <= '9') ||
            (len == 3 && (c == 's' || c == 'S'))) return 1;
    }
    return 0;
}

/* Return 1 only when every byte has been checked. Large matching samples
 * give -2 (INCONCLUSIVE), never "identical". Return 0 on proven mismatch and
 * -1 on an I/O error. The scan remains bounded even with terabyte libraries. */
static int read_equal_span(int a, int b, off_t off, size_t n, int *io_error) {
    unsigned char x[COMPARE_BLOCK_BYTES], y[COMPARE_BLOCK_BYTES];
    while (n) {
        size_t k = n < sizeof(x) ? n : sizeof(x);
        ssize_t ra = pread(a, x, k, off), rb = pread(b, y, k, off);
        if (ra != (ssize_t)k || rb != (ssize_t)k) { *io_error = 1; return 0; }
        if (memcmp(x, y, k)) return 0;
        off += (off_t)k;
        n -= k;
    }
    return 1;
}

static int compare_update_bytes(const char *left, const char *right) {
    int a = open(left, O_RDONLY | O_NOFOLLOW);
    if (a < 0) return -1;
    int b = open(right, O_RDONLY | O_NOFOLLOW);
    if (b < 0) { close(a); return -1; }
    struct stat sa, sb, ea, eb;
    int result = -1;
    if (fstat(a, &sa) || fstat(b, &sb) ||
        !S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode) ||
        sa.st_size < 0 || sb.st_size < 0) goto out;
    if (sa.st_size != sb.st_size) { result = 0; goto out; }
    int io_error = 0;
    if ((uint64_t)sa.st_size <= EXACT_COMPARE_MAX_BYTES) {
        result = read_equal_span(a, b, 0, (size_t)sa.st_size, &io_error) ? 1 : 0;
    } else {
        /* Compare three bounded regions. Matching samples cannot establish
         * full-file equality, especially for modified or repackaged PKGs. */
        off_t last = sa.st_size - COMPARE_BLOCK_BYTES;
        off_t mid = sa.st_size / 2 - COMPARE_BLOCK_BYTES / 2;
        result = (!read_equal_span(a, b, 0, COMPARE_BLOCK_BYTES, &io_error) ||
                  !read_equal_span(a, b, mid, COMPARE_BLOCK_BYTES, &io_error) ||
                  !read_equal_span(a, b, last, COMPARE_BLOCK_BYTES, &io_error)) ? 0 : -2;
    }
    if (io_error || fstat(a, &ea) || fstat(b, &eb) ||
        sa.st_size != ea.st_size || sb.st_size != eb.st_size ||
        sa.st_mtime != ea.st_mtime || sb.st_mtime != eb.st_mtime) result = -1;
out:
    close(a);
    close(b);
    return result;
}

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
                        char name[128], char ver[16], unsigned long long *size,
                        char content_id[37]) {
    *category = 0;
    pkg_t pkg;
    if (pkg_open(&pkg, path)) return -1;
    sfo_t sfo;
    int ok = !read_sfo(&pkg, &sfo);
    const sfo_item_t *cat = ok ? sfo_get(&sfo, "CATEGORY") : NULL;
    int kind = cat && !cat->is_int && !strcmp(cat->sval, "gd") ? 1 :
               cat && !cat->is_int && !strcmp(cat->sval, "gp") ? 2 : 0;
    if (cat && !cat->is_int && !strcmp(cat->sval, "ac")) *category = 3;
    ok = kind && strlen(pkg.title_id) == 9 && !strncmp(pkg.title_id, "CUSA", 4) && pkg_fake_key(&pkg);
    if (ok) {
        const sfo_item_t *n = sfo_get(&sfo, "TITLE"), *v = sfo_get(&sfo, "APP_VER");
        snprintf(tid, 10, "%s", pkg.title_id);
        snprintf(content_id, 37, "%s", pkg.content_id);
        *category = kind;
        snprintf(name, 128, "%s", n && !n->is_int ? n->sval : pkg.title_id);
        /* Missing APP_VER is not safe for choosing the newest update. */
        snprintf(ver, 16, "%s", v && !v->is_int ? v->sval : "");
        *size = (unsigned long long)pkg.pkg_size;
    }
    pkg_close(&pkg);
    return ok ? 0 : -1;
}

static void inspect_pkg(const char *root, const char *path) {
    char tid[10], name[128], ver[16], content_id[37];
    unsigned long long size = 0;
    int kind = 0;
    if (describe_pkg(path, tid, &kind, name, ver, &size, content_id)) {
        if (kind == 3) {
            g_scan_dlc_ignored++;
            if (g_scan_dlc_ignored <= 8)
                printf("[safe][pkg] CATEGORY=ac DLC ignored: %s\n", path);
        } else {
            g_scan_unreadable++;
            if (g_scan_unreadable <= 8)
                printf("[safe][pkg] not indexable (unreadable, retail or unsupported): %s\n", path);
        }
        return;
    }
    printf("[safe][pkg] type=%s CUSA=%s APP_VER=%s content_id=%s pkg_size=%llu: %s\n",
           kind == 1 ? "base(gd)" : "patch(gp)", tid, ver, content_id, size, path);
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
        snprintf(t->content_id, sizeof(t->content_id), "%s", content_id);
        snprintf(t->dir, sizeof(t->dir), "%s", path);
        char *slash = strrchr(t->dir, '/');
        if (slash) *slash = 0;
    } else {
        if (t->update_count >= MAX_UPDATE_CANDIDATES) {
            t->update_overflow = 1;
            t->patch_conflict = 1;
            printf("[safe][scan] %s too many updates; BLOCKED\n", tid);
            return;
        }
        update_candidate_t *c = &t->patches[t->update_count++];
        snprintf(c->path, sizeof(c->path), "%s", path);
        snprintf(c->version, sizeof(c->version), "%s", ver);
        snprintf(c->content_id, sizeof(c->content_id), "%s", content_id);
        c->size = size;
        c->suspicious_label = has_addon_label(path);
        if (c->suspicious_label)
            printf("[safe][update] %s SUSPECT DLC/add-on label despite CATEGORY=gp: %s\n", tid, path);
    }
}

/* Choose the newest compatible patch from all directories, not the first one.
 * Equal highest versions, invalid versions, or truncated lists are blocked.
 * The base and update must have the same complete content_id, not just CUSA. */
static void resolve_updates(title_t *t) {
    t->patch_path[0] = t->patch_version[0] = 0;
    t->patch_size = 0;
    t->invalid_updates = t->incompatible_updates = 0;
    t->suspicious_updates = t->verified_duplicate_updates = t->unverified_duplicate_updates = 0;
    t->patch_conflict = t->update_overflow;
    int best = -1, ties = 0;
    for (int i = 0; i < t->update_count; ++i) {
        update_candidate_t *c = &t->patches[i];
        if (!t->app_path[0]) continue;
        if (c->suspicious_label) {
            t->suspicious_updates++;
            printf("[safe][update] %s possible DLC/combined package requires manual inspection: %s\n",
                   t->tid, c->path);
            continue;
        }
        if (!t->content_id[0] || !c->content_id[0] ||
            strcmp(t->content_id, c->content_id)) {
            t->incompatible_updates++;
            printf("[safe][update] %s skipped INCOMPATIBLE base_cid=%s update_cid=%s: %s\n",
                   t->tid, t->content_id, c->content_id, c->path);
            continue;
        }
        int parts[4];
        if (ps4mount_parse_app_ver(c->version, parts)) {
            t->invalid_updates++;
            printf("[safe][update] %s UNREADABLE APP_VER=%s: %s\n", t->tid, c->version, c->path);
            continue;
        }
        int cmp = best < 0 ? 1 : ps4mount_compare_app_ver(c->version, t->patches[best].version);
        if (cmp > 0) { best = i; ties = 1; }
        else if (cmp == 0) {
            ties++;
            /* Deterministic name used only if all tied packages match exactly. */
            if (strcmp(c->path, t->patches[best].path) < 0) best = i;
        }
    }
    if (ties > 1 && best >= 0) {
        int different = 0;
        for (int i = 0; i < t->update_count; ++i) {
            if (i == best || t->patches[i].suspicious_label ||
                strcmp(t->content_id, t->patches[i].content_id) ||
                ps4mount_compare_app_ver(t->patches[i].version, t->patches[best].version)) continue;
            int equal = compare_update_bytes(t->patches[best].path, t->patches[i].path);
            if (equal == 1) {
                t->verified_duplicate_updates++;
                printf("[safe][update] %s VERIFIED byte-identical update: %s == %s\n",
                       t->tid, t->patches[best].path, t->patches[i].path);
            } else if (equal == 0) {
                different++;
                printf("[safe][update] %s DIFFERENT files with same APP_VER=%s: %s vs %s\n",
                       t->tid, t->patches[best].version, t->patches[best].path, t->patches[i].path);
            } else {
                t->unverified_duplicate_updates++;
                printf("[safe][update] %s UNVERIFIED same-version packages (%s); BLOCKED: %s vs %s\n",
                       t->tid, equal == -2 ? "too large for bounded full comparison" : "read error",
                       t->patches[best].path, t->patches[i].path);
            }
        }
        if (different || t->unverified_duplicate_updates) t->patch_conflict = 1;
    }
    if (t->suspicious_updates || t->invalid_updates || t->update_overflow)
        t->patch_conflict = 1;
    if (t->patch_conflict) {
        printf("[safe][update] %s BLOCKED: suspects=%d invalid=%d top_ties=%d verified_duplicates=%d unverified=%d overflow=%d\n",
               t->tid, t->suspicious_updates, t->invalid_updates, ties,
               t->verified_duplicate_updates, t->unverified_duplicate_updates, t->update_overflow);
        return;
    }
    if (best >= 0) {
        update_candidate_t *c = &t->patches[best];
        snprintf(t->patch_path, sizeof(t->patch_path), "%s", c->path);
        snprintf(t->patch_version, sizeof(t->patch_version), "%s", c->version);
        t->patch_size = c->size;
        printf("[safe][update] %s selected APP_VER=%s among %d update(s), verified_duplicates=%d: %s\n",
               t->tid, c->version, t->update_count, t->verified_duplicate_updates, c->path);
    } else if (t->update_count && t->app_path[0]) {
        t->patch_conflict = 1;
        printf("[safe][update] %s no compatible update; BLOCKED\n", t->tid);
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
    char id[10], name[128], ver[16], cid[37]; int kind; unsigned long long bytes;
    if (describe_pkg(t->app_path, id, &kind, name, ver, &bytes, cid) ||
        kind != 1 || strcmp(id, t->tid) || strcmp(cid, t->content_id) || bytes != t->size) return -1;
    if (t->patch_path[0] &&
        (describe_pkg(t->patch_path, id, &kind, name, ver, &bytes, cid) ||
         kind != 2 || strcmp(id, t->tid) || strcmp(cid, t->content_id) ||
         strcmp(ver, t->patch_version) || bytes != t->patch_size)) return -1;
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

/* Phase 3A: READ-ONLY preflight for ONE title. No mkdir/rename/write here.
 * Deliberately separate from the historical prepare_title() write routine:
 * the plan is informational only, never an authorization to move PKGs. */
typedef struct {
    char destination[PATH_MAX];
    char app_destination[PATH_MAX];
    char patch_destination[PATH_MAX];
    int app_move, patch_move, create_library, create_folder;
} title_plan_t;

static int plan_error(char *why, size_t cap, const char *message) {
    if (cap) snprintf(why, cap, "%s", message);
    return -1;
}

static int within_root(const char *root, const char *path) {
    size_t n = strlen(root);
    return n && !strncmp(root, path, n) && path[n] == '/';
}

/* Check each component with lstat, not stat, to reject links anywhere below
 * the USB mountpoint. Also refuse a nested mount on another filesystem. */
static int checked_source(const char *root, const char *path, dev_t volume) {
    if (!within_root(root, path)) return -1;
    char component[PATH_MAX];
    size_t n = strlen(root);
    if (n >= sizeof(component)) return -1;
    memcpy(component, root, n + 1);
    for (const char *p = path + n + 1; ; p++) {
        if (*p && *p != '/') continue;
        size_t k = (size_t)(p - path);
        if (k >= sizeof(component)) return -1;
        memcpy(component, path, k);
        component[k] = 0;
        struct stat st;
        if (lstat(component, &st) || st.st_dev != volume) return -1;
        if (!*p) return S_ISREG(st.st_mode) ? 0 : -1;
        if (!S_ISDIR(st.st_mode)) return -1;
    }
}

/* Missing future directories are fine; existing directories must be real,
 * on the same volume, and not links or separate mounts. */
static int checked_folder(const char *path, dev_t volume, int *missing) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        *missing = 0;
        return S_ISDIR(st.st_mode) && st.st_dev == volume ? 0 : -1;
    }
    if (errno != ENOENT) return -1;
    *missing = 1;
    return 0;
}

static int destination_available(const char *source, const char *destination) {
    if (!strcmp(source, destination)) return 0;
    struct stat st;
    if (lstat(destination, &st) == 0) return -1; /* do not overwrite anything */
    return errno == ENOENT ? 0 : -1;
}

static int plan_title(const title_t *t, title_plan_t *plan, char *why, size_t cap) {
    memset(plan, 0, sizeof(*plan));
    if (!t || !t->app_path[0]) return plan_error(why, cap, "Base PKG not indexed");
    if (t->conflict || t->patch_conflict)
        return plan_error(why, cap, "Game or update needs manual review; preparation blocked");
    /* With an unselected update candidate we cannot silently omit a patch. */
    if (t->update_count && !t->patch_path[0])
        return plan_error(why, cap, "Update candidates exist but none is approved");
    struct stat rootst;
    if (lstat(t->root, &rootst) || !S_ISDIR(rootst.st_mode))
        return plan_error(why, cap, "USB drive is unavailable");
    if (checked_source(t->root, t->app_path, rootst.st_dev))
        return plan_error(why, cap, "Base is no longer a regular file on this USB drive");
    if (t->patch_path[0] && checked_source(t->root, t->patch_path, rootst.st_dev))
        return plan_error(why, cap, "Update is no longer a regular file on this USB drive");

    char library[PATH_MAX], canonical[PATH_MAX];
    if (snprintf(library, sizeof(library), "%s/ps4", t->root) >= (int)sizeof(library) ||
        snprintf(canonical, sizeof(canonical), "%s/%s", library, t->tid) >= (int)sizeof(canonical))
        return plan_error(why, cap, "Destination path is too long");
    /* Preserve a title already stored in the legacy /ps4/CUSA directory. */
    char legacy_app[PATH_MAX];
    if (snprintf(legacy_app, sizeof(legacy_app), "%s/app.pkg", canonical) >= (int)sizeof(legacy_app))
        return plan_error(why, cap, "Legacy destination path is too long");
    if (!strcmp(t->app_path, legacy_app)) {
        snprintf(plan->destination, sizeof(plan->destination), "%s", canonical);
    } else {
        char folder[128]; safe_folder_name(t, folder);
        if (snprintf(plan->destination, sizeof(plan->destination), "%s/%s", library, folder) >=
            (int)sizeof(plan->destination))
            return plan_error(why, cap, "Descriptive destination path is too long");
    }
    if (snprintf(plan->app_destination, sizeof(plan->app_destination), "%s/app.pkg", plan->destination) >=
        (int)sizeof(plan->app_destination) ||
        snprintf(plan->patch_destination, sizeof(plan->patch_destination), "%s/patch.pkg", plan->destination) >=
        (int)sizeof(plan->patch_destination))
        return plan_error(why, cap, "Output filename is too long");
    if (checked_folder(library, rootst.st_dev, &plan->create_library) ||
        (plan->create_library ? (plan->create_folder = 1, 0) :
        checked_folder(plan->destination, rootst.st_dev, &plan->create_folder)))
        return plan_error(why, cap, "Destination directory is not safe (link or different filesystem)");
    plan->app_move = strcmp(t->app_path, plan->app_destination) != 0;
    plan->patch_move = t->patch_path[0] && strcmp(t->patch_path, plan->patch_destination) != 0;
    if (destination_available(t->app_path, plan->app_destination) ||
        (t->patch_path[0] && destination_available(t->patch_path, plan->patch_destination)))
        return plan_error(why, cap, "Destination exists or cannot be inspected; no overwrite allowed");
    if (cap) snprintf(why, cap, "Plan ready: no files changed");
    return 0;
}

static void scan_root(const char *root) {
    g_scan_dlc_ignored = g_scan_unreadable = 0;
    catalog_forget_root(root);
    scan_nested(root, root, 0);
    for (int i = 0; i < g_title_count; ++i)
        if (!strcmp(root, g_titles[i].root)) resolve_updates(&g_titles[i]);
    int games = 0, conflicts = 0, updates = 0, total_candidates = 0, ignored_incompatible = 0;
    int suspicious = 0, duplicates_verified = 0, duplicates_unverified = 0;
    for (int i = 0; i < g_title_count; i++) {
        title_t *t = &g_titles[i];
        if (strcmp(root, t->root) || !t->app_path[0]) continue;
        games++;
        if (t->conflict || t->patch_conflict) conflicts++;
        if (t->patch_path[0]) updates++;
        total_candidates += t->update_count;
        ignored_incompatible += t->incompatible_updates;
        suspicious += t->suspicious_updates;
        duplicates_verified += t->verified_duplicate_updates;
        duplicates_unverified += t->unverified_duplicate_updates;
    }
    printf("[safe][scan] %s: games=%d selected_updates=%d candidates=%d incompatible=%d conflicts=%d suspects=%d verified_duplicates=%d unverified_duplicates=%d ignored_dlc=%d unreadable=%d; NO files moved, NO games registered\n",
           root, games, updates, total_candidates, ignored_incompatible, conflicts,
           suspicious, duplicates_verified, duplicates_unverified, g_scan_dlc_ignored, g_scan_unreadable);
    notify("%d games indexed on %s (read-only); registration LOCKED", games, root);
}

static void scan_all(void) {
    char root[32];
    /* ext* may contain PS5 system app mounts; only index USB game drives. */
    for (int i = 0; i < 8; i++) {
        snprintf(root, sizeof(root), "/mnt/usb%d", i);
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
        buf_printf(out, ",\"update_version\":"); json_str(out, t->patch_version);
        buf_printf(out, ",\"update_candidates\":%d", t->update_count);
        buf_printf(out, ",\"incompatible_updates\":%d", t->incompatible_updates);
        buf_printf(out, ",\"suspicious_updates\":%d,\"verified_duplicate_updates\":%d,\"unverified_duplicate_updates\":%d",
                   t->suspicious_updates, t->verified_duplicate_updates, t->unverified_duplicate_updates);
        buf_printf(out, ",\"size\":%llu,\"has_patch\":%s,\"state\":\"%s\",\"mount\":\"\",\"source\":",
                   t->size + t->patch_size, t->patch_path[0] ? "true" : "false", state);
        json_str(out, t->app_path);
        buf_printf(out, ",\"update_source\":");
        json_str(out, t->patch_path);
        buf_printf(out, ",\"update_issue\":");
        json_str(out, t->conflict ? "Duplicate base PKGs for this CUSA; no action allowed" :
                       t->patch_conflict && t->suspicious_updates ? "Suspicious DLC/add-on label in update source; manual inspection required" :
                       t->patch_conflict && t->unverified_duplicate_updates ? "Same-version large/unreadable files cannot be verified identical; manual review required" :
                       t->patch_conflict ? "Same-version different PKGs or missing valid update; review required" :
                       t->incompatible_updates ? "Incompatible updates skipped (content ID mismatch)" : "");
        buf_printf(out, "}");
        first = 0;
    }
    buf_printf(out, "]}");
}

/* Authorization is per-process, single-use and bound to the latest preview.
 * Fetching another preview invalidates any previous confirmation token. */
static struct {
    char nonce[33], tid[10], root[32], app_path[PATH_MAX], patch_path[PATH_MAX];
    struct stat app_stat, patch_stat;
    time_t issued;
    int pending_recovery, usb_number;
} g_move_auth;

static void auth_clear(void) { memset(&g_move_auth, 0, sizeof(g_move_auth)); }

static int auth_nonce(void) {
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < sizeof(b)) {
        ssize_t k = read(fd, b + n, sizeof(b) - n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { close(fd); return -1; }
        n += (size_t)k;
    }
    close(fd);
    for (int i = 0; i < 16; i++)
        snprintf(g_move_auth.nonce + i * 2, 3, "%02x", b[i]);
    return 0;
}
static int auth_matches(const char *nonce, int recovery) {
    if (!nonce || strlen(nonce) != 32 || !g_move_auth.nonce[0] ||
        g_move_auth.pending_recovery != recovery ||
        time(NULL) < g_move_auth.issued || time(NULL) - g_move_auth.issued > 120)
        return 0;
    unsigned char mismatch = 0;
    for (int i = 0; i < 32; ++i) mismatch |= (unsigned char)(nonce[i] ^ g_move_auth.nonce[i]);
    return mismatch == 0;
}
static int same_stat(const char *path, const struct stat *old) {
    struct stat now;
    return !lstat(path, &now) && S_ISREG(now.st_mode) &&
           now.st_dev == old->st_dev && now.st_ino == old->st_ino &&
           now.st_size == old->st_size && now.st_mtime == old->st_mtime &&
           now.st_ctime == old->st_ctime;
}
static int pkg_matches_catalog(const title_t *t, char *why, size_t cap) {
    char id[10], name[128], version[16], cid[37];
    unsigned long long size;
    int kind;
    if (describe_pkg(t->app_path, id, &kind, name, version, &size, cid) ||
        kind != 1 || strcmp(id, t->tid) || strcmp(cid, t->content_id) || size != t->size)
        return plan_error(why, cap, "Base PKG metadata changed; rescan required");
    if (t->patch_path[0] &&
        (describe_pkg(t->patch_path, id, &kind, name, version, &size, cid) ||
         kind != 2 || strcmp(id, t->tid) || strcmp(cid, t->content_id) ||
         strcmp(version, t->patch_version) || size != t->patch_size))
        return plan_error(why, cap, "Update PKG metadata changed; rescan required");
    return 0;
}
static int supported_usb(const title_t *t) {
    return t && !strncmp(t->root, "/mnt/usb", 8) &&
           t->root[8] >= '0' && t->root[8] <= '7' && !t->root[9] && is_mountpoint(t->root);
}

/* Phase 4A: per-CUSA READ-ONLY mount/registration prerequisites.  Never
 * call the legacy register_title(), mount_title(), place_files() or any
 * system-writing function from this endpoint. Passing this check does NOT
 * mean the title is runnable: system integration remains unimplemented. */
enum {
    PH4_CATALOG       = 1u << 0,
    PH4_CONFLICT_FREE = 1u << 1,
    PH4_USB_MOUNTED   = 1u << 2,
    PH4_JOURNAL_CLEAR = 1u << 3,
    PH4_NOT_INTERNAL  = 1u << 4,
    PH4_SAFE_PATHS    = 1u << 5,
    PH4_ORGANIZED     = 1u << 6,
    PH4_METADATA_OK   = 1u << 7,
    PH4_DB_READABLE   = 1u << 8,
    PH4_DB_ABSENT     = 1u << 9,
};
#define PH4_REQUIRED ((1u << 10) - 1u)

static const char *phase4_first_blocker(unsigned int checks) {
    if (!(checks & PH4_CATALOG)) return "Game not found: scan USB and retry";
    if (!(checks & PH4_CONFLICT_FREE)) return "Game/update conflict: review candidates first";
    if (!(checks & PH4_USB_MOUNTED)) return "USB is not a supported mounted drive";
    if (!(checks & PH4_JOURNAL_CLEAR)) return "USB has an unfinished/unreadable organization journal; inspect USB recovery";
    if (!(checks & PH4_NOT_INTERNAL)) return "Game appears installed internally: refusing conflicting registration";
    if (!(checks & PH4_SAFE_PATHS)) return "Cannot verify PKG paths or target directory; check Phase 3B preview";
    if (!(checks & PH4_ORGANIZED)) return "PKGs are not yet at their Phase 3B destination; organize ONE title first";
    if (!(checks & PH4_METADATA_OK)) return "PKG metadata changed or cannot be verified; rescan and inspect";
    if (!(checks & PH4_DB_READABLE)) return "Cannot read PS5 application database safely";
    if (!(checks & PH4_DB_ABSENT)) return "CUSA is already registered; do not overwrite or re-register";
    return "USB/metadata preflight passed. Launch/registration is NOT enabled.";
}
/* Phase 4B: ONE-TITLE, OPT-IN, READ-ONLY nullfs mount experiment.
 * It does not call sceAppInstUtilAppInstallAll, register_title, place_files,
 * write metadata, or modify SQLite. It is NOT a launch operation.
 * Real console verification is required before enabling registration. */
static int phase4_ready(unsigned int checks);
static struct {
    int active, attempted, made_app_dir, made_patch_dir;
    char tid[10], dir[PATH_MAX];
} g_probe;
static struct {
    char token[33], tid[10], mode[12], app_path[PATH_MAX], patch_path[PATH_MAX];
    struct stat app_stat, patch_stat;
    time_t issued;
} g_probe_auth;

static void probe_auth_clear(void) { memset(&g_probe_auth, 0, sizeof(g_probe_auth)); }
static int probe_auth_generate(const char *tid, const char *mode, const title_t *t) {
    unsigned char raw[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < sizeof(raw)) {
        ssize_t n = read(fd, raw + got, sizeof(raw) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    probe_auth_clear();
    for (int i = 0; i < 16; ++i)
        snprintf(g_probe_auth.token + i * 2, 3, "%02x", raw[i]);
    snprintf(g_probe_auth.tid, sizeof(g_probe_auth.tid), "%s", tid);
    snprintf(g_probe_auth.mode, sizeof(g_probe_auth.mode), "%s", mode);
    if (t && !strcmp(mode, "mount")) {
        snprintf(g_probe_auth.app_path, sizeof(g_probe_auth.app_path), "%s", t->app_path);
        snprintf(g_probe_auth.patch_path, sizeof(g_probe_auth.patch_path), "%s", t->patch_path);
        if (lstat(t->app_path, &g_probe_auth.app_stat) ||
            (t->patch_path[0] && lstat(t->patch_path, &g_probe_auth.patch_stat))) {
            probe_auth_clear(); return -1;
        }
    }
    g_probe_auth.issued = time(NULL);
    return 0;
}
static int probe_auth_consume(const char *tid, const char *mode, const char *token,
                              struct stat *app, struct stat *patch,
                              char *app_path, char *patch_path) {
    int ok = token && strlen(token) == 32 && g_probe_auth.token[0] &&
             !strcmp(tid, g_probe_auth.tid) && !strcmp(mode, g_probe_auth.mode) &&
             time(NULL) >= g_probe_auth.issued && time(NULL) - g_probe_auth.issued <= 120;
    unsigned char diff = 0;
    if (ok) for (int i = 0; i < 32; ++i)
        diff |= (unsigned char)(token[i] ^ g_probe_auth.token[i]);
    ok = ok && diff == 0;
    if (ok && app) {
        *app = g_probe_auth.app_stat;
        *patch = g_probe_auth.patch_stat;
        snprintf(app_path, PATH_MAX, "%s", g_probe_auth.app_path);
        snprintf(patch_path, PATH_MAX, "%s", g_probe_auth.patch_path);
    }
    probe_auth_clear(); /* ONE USE, even on a mismatch. */
    return ok;
}

/* Only create a nullfs mount over an absent or EMPTY real directory. Never
 * obscure existing files, a symlink, or someone else's mount. */
static int probe_empty_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int empty = 1;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = 0; break; }
    }
    int rc = closedir(d);
    return rc ? -1 : empty;
}
static int probe_safe_target(const char *path, const char *parent, const char *source, int *create) {
    struct stat st;
    if (lstat("/user", &st) || !S_ISDIR(st.st_mode) ||
        lstat(parent, &st) || !S_ISDIR(st.st_mode)) return -1;
    if (lstat(path, &st)) {
        if (errno != ENOENT) return -1;
        *create = 1;
        return 0;
    }
    if (!S_ISDIR(st.st_mode) || top_layer(path, source) != 0 || probe_empty_dir(path) != 1)
        return -1;
    *create = 0;
    return 0;
}
static int phase4_compute(title_t *t, title_plan_t *plan, char *detail, size_t cap,
                          int *db_status) {
    unsigned int checks = 0;
    *db_status = -2;
    if (t && t->app_path[0]) checks |= PH4_CATALOG;
    if ((checks & PH4_CATALOG) && !t->conflict && !t->patch_conflict &&
        !(t->update_count && !t->patch_path[0])) checks |= PH4_CONFLICT_FREE;
    if ((checks & PH4_CONFLICT_FREE) && supported_usb(t)) checks |= PH4_USB_MOUNTED;
    if ((checks & PH4_USB_MOUNTED) && org_pending(t->root) == 0) checks |= PH4_JOURNAL_CLEAR;
    if ((checks & PH4_JOURNAL_CLEAR) && !installed_internally(t->tid)) checks |= PH4_NOT_INTERNAL;
    if ((checks & PH4_NOT_INTERNAL) && !plan_title(t, plan, detail, cap)) {
        checks |= PH4_SAFE_PATHS;
        if (!plan->app_move && !plan->patch_move && !plan->create_library && !plan->create_folder)
            checks |= PH4_ORGANIZED;
    }
    if ((checks & PH4_ORGANIZED) && !pkg_matches_catalog(t, detail, cap))
        checks |= PH4_METADATA_OK;
    if ((checks & PH4_METADATA_OK) && (*db_status = db_registered_status(t->tid)) >= 0) {
        checks |= PH4_DB_READABLE;
        if (*db_status == 0) checks |= PH4_DB_ABSENT;
    }
    return (int)checks;
}

/* Called by an explicit UI preview. Never mounts in this handler. */
int app_probe_preview_json(const char *tid, buf_t *out) {
    title_t *t = catalog_find(tid);
    title_plan_t plan = {0};
    char detail[256] = "";
    int db_status = -2;
    unsigned int checks = 0;
    int can_mount = 0, can_release = 0;
    probe_auth_clear();
    if (g_probe.active) {
        can_release = !strcmp(g_probe.tid, tid);
    } else if (!g_probe.attempted) {
        checks = (unsigned int)phase4_compute(t, &plan, detail, sizeof(detail), &db_status);
        if (phase4_ready(checks)) {
            int make_app = 0, make_patch = 0;
            char app[64], patch[64];
            snprintf(app, sizeof(app), "/user/app/%.9s", tid);
            snprintf(patch, sizeof(patch), "/user/patch/%.9s", tid);
            can_mount = !probe_safe_target(app, "/user/app", t->dir, &make_app) &&
                        !probe_safe_target(patch, "/user/patch", t->dir, &make_patch);
        }
    }
    const char *mode = can_release ? "release" : can_mount ? "mount" : "";
    if (mode[0] && probe_auth_generate(tid, mode, t)) { can_mount = can_release = 0; mode = ""; }
    buf_printf(out, "{\"ok\":true,\"title_id\":"); json_str(out, tid);
    buf_printf(out, ",\"can_mount\":%s,\"can_release\":%s,\"attempted\":%s,\"read_only\":true,\"registration_enabled\":false,\"launchable\":false,\"message\":",
               can_mount ? "true" : "false", can_release ? "true" : "false", g_probe.attempted ? "true" : "false");
    const char *message = g_probe.active && !can_release ? "Another title has the active probe; release it first" :
        can_release ? "Read-only mount active. Release to restore unmounted paths" :
        g_probe.attempted ? "One mount attempt per daemon session. Restart before another attempt" :
        !phase4_ready(checks) ? phase4_first_blocker(checks) :
        "Target mountpoint is occupied, nonempty, a symlink, or cannot be inspected";
    if (can_mount) message = "Ready for a SINGLE temporary read-only nullfs mount. NOT launchable.";
    json_str(out, message);
    buf_printf(out, ",\"confirm_token\":"); json_str(out, mode[0] ? g_probe_auth.token : "");
    buf_printf(out, "}");
    printf("[safe][mount-probe] %s PREVIEW %s: %s (NO DB write, NO registration)\n",
           tid, mode[0] ? mode : "blocked", message);
    return 0;
}

/* A probe can be released only when top mount is exactly our source folder. */
static int probe_layer(const char *target, const char *source) {
    struct stat st;
    if (lstat(target, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    return top_layer(target, source);
}
static int probe_release_internal(char *msg, size_t cap) {
    if (!g_probe.active) return plan_error(msg, cap, "No active mount probe");
    int failed = 0;
    char app[64], patch[64];
    snprintf(app, sizeof(app), "/user/app/%.9s", g_probe.tid);
    snprintf(patch, sizeof(patch), "/user/patch/%.9s", g_probe.tid);
    int pa = probe_layer(app, g_probe.dir), pp = probe_layer(patch, g_probe.dir);
    if (pa < 0 || pp < 0) {
        snprintf(msg, cap, "Unknown or foreign mount detected; no mount touched. Inspect before retry");
        return -1;
    }
    if (pp > 0 && unmount_owned_layer(patch, g_probe.dir)) failed = 1;
    if (pa > 0 && unmount_owned_layer(app, g_probe.dir)) failed = 1;
    if (failed) {
        snprintf(msg, cap, "At least one read-only probe mount could not be released; do NOT retry");
        return -1;
    }
    if (g_probe.made_patch_dir) (void)rmdir(patch); /* only succeeds when empty */
    if (g_probe.made_app_dir) (void)rmdir(app);
    printf("[safe][mount-probe] %s RELEASED read-only layer(s); NO registration\n", g_probe.tid);
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.attempted = 1;
    snprintf(msg, cap, "Read-only mount probe released. Game was NOT registered or launched");
    return 0;
}
int app_probe_release(const char *tid, const char *token, char *msg, size_t cap) {
    if (!probe_auth_consume(tid, "release", token, NULL, NULL, NULL, NULL))
        return plan_error(msg, cap, "Invalid/expired single-use release confirmation; refresh preview");
    if (!g_probe.active || strcmp(g_probe.tid, tid))
        return plan_error(msg, cap, "No matching active probe");
    return probe_release_internal(msg, cap);
}
int app_probe_mount(const char *tid, const char *token, char *msg, size_t cap) {
    struct stat a, p;
    char app_path[PATH_MAX], patch_path[PATH_MAX];
    if (!probe_auth_consume(tid, "mount", token, &a, &p, app_path, patch_path))
        return plan_error(msg, cap, "Invalid/expired single-use probe confirmation; refresh preview");
    if (g_probe.active || g_probe.attempted)
        return plan_error(msg, cap, "A mount was already attempted this session");
    g_probe.attempted = 1; /* fail-closed: no second attempt, even on failure */
    title_t *t = catalog_find(tid);
    title_plan_t plan = {0};
    char detail[256] = "";
    int db_status = -2;
    if (!phase4_ready((unsigned int)phase4_compute(t, &plan, detail, sizeof(detail), &db_status)) ||
        !t || strcmp(app_path, t->app_path) || strcmp(patch_path, t->patch_path) ||
        !same_stat(t->app_path, &a) || (t->patch_path[0] && !same_stat(t->patch_path, &p)))
        return plan_error(msg, cap, "Title changed or failed preflight; NO mounts made");
    char app_target[64], patch_target[64];
    snprintf(app_target, sizeof(app_target), "/user/app/%.9s", tid);
    snprintf(patch_target, sizeof(patch_target), "/user/patch/%.9s", tid);
    int made_app = 0, made_patch = 0;
    if (probe_safe_target(app_target, "/user/app", t->dir, &made_app) ||
        probe_safe_target(patch_target, "/user/patch", t->dir, &made_patch))
        return plan_error(msg, cap, "Mount target not empty/available; NO mounts made");
    /* Create only the directories confirmed absent. Reject races or replacements. */
    if (made_app && mkdir(app_target, 0777))
        return plan_error(msg, cap, "App mountpoint changed during confirmation; refused");
    if (t->patch_path[0] && made_patch && mkdir(patch_target, 0777)) {
        if (made_app) (void)rmdir(app_target);
        return plan_error(msg, cap, "Patch mountpoint changed during confirmation; refused");
    }
    snprintf(g_probe.tid, sizeof(g_probe.tid), "%s", tid);
    snprintf(g_probe.dir, sizeof(g_probe.dir), "%s", t->dir);
    g_probe.active = 1;
    /* Record any targets we created, so an interrupted second mount can be cleaned. */
    g_probe.made_app_dir = made_app;
    g_probe.made_patch_dir = t->patch_path[0] && made_patch;
    int rc = mount_dir(t->dir, app_target, 0); /* ro only: nmount(MNT_RDONLY) */
    if (!rc && t->patch_path[0]) rc = mount_dir(t->dir, patch_target, 0);
    if (!rc && (probe_layer(app_target, t->dir) != 1 ||
                (t->patch_path[0] && probe_layer(patch_target, t->dir) != 1))) {
        printf("[safe][mount-probe] %s RO source/target verification FAILED\n", tid);
        rc = -1;
    }
    if (rc) {
        printf("[safe][mount-probe] %s mount FAILED; attempting owned-layer cleanup\n", tid);
        char rollback[256];
        if (probe_release_internal(rollback, sizeof(rollback))) {
            snprintf(msg, cap, "Mount failed and cleanup is INCOMPLETE. Inspect logs; do not retry");
            return -1;
        }
        return plan_error(msg, cap, "Read-only nullfs mount refused/failed; cleanup attempted. See log");
    }
    printf("[safe][mount-probe] %s MOUNTED READ-ONLY: %s (no registration, no game launch)\n", tid, t->dir);
    snprintf(msg, cap, "Temporary READ-ONLY probe mounted for %s. Now RELEASE it in panel; game NOT registered", tid);
    return 0;
}

static int phase4_ready(unsigned int checks) {
    return (checks & PH4_REQUIRED) == PH4_REQUIRED;
}
static void phase4_check_bool(buf_t *out, const char *key, unsigned int flags, unsigned int bit, int comma) {
    buf_printf(out, "%s\"%s\":%s", comma ? "," : "", key, (flags & bit) ? "true" : "false");
}

/* An informational GET only: lstat, PKG header reads and SELECT from a
 * read-only SQLite handle. No mkdir, rename, mount, system write or install. */
int app_launch_check_json(const char *tid, buf_t *out) {
    title_t *t = catalog_find(tid);
    char detail[256] = "";
    int db_status = -2;
    title_plan_t plan = {0};
    unsigned int checks = (unsigned int)phase4_compute(t, &plan, detail, sizeof(detail), &db_status);
    int ready = phase4_ready(checks);
    const char *message = phase4_first_blocker(checks);
    buf_printf(out, "{\"ok\":true,\"title_id\":"); json_str(out, tid);
    buf_printf(out, ",\"preflight_passed\":%s,\"read_only\":true,"
                    "\"launchable\":false,\"registration_enabled\":false,"
                    "\"message\":", ready ? "true" : "false");
    json_str(out, message);
    buf_printf(out, ",\"detail\":"); json_str(out, detail);
    buf_printf(out, ",\"database_status\":");
    json_str(out, db_status == 0 ? "not_registered" : db_status == 1 ? "already_registered" :
                  db_status == -1 ? "unavailable" : "not_checked");
    buf_printf(out, ",\"app_path\":"); json_str(out, t ? t->app_path : "");
    buf_printf(out, ",\"update_path\":"); json_str(out, t ? t->patch_path : "");
    buf_printf(out, ",\"expected_app_path\":"); json_str(out, plan.app_destination);
    buf_printf(out, ",\"expected_update_path\":");
    json_str(out, t && t->patch_path[0] ? plan.patch_destination : "");
    buf_printf(out, ",\"proposed_app_mount\":");
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%.9s", tid);
    json_str(out, target);
    buf_printf(out, ",\"proposed_patch_mount\":");
    if (t && t->patch_path[0]) snprintf(target, sizeof(target), "/user/patch/%.9s", tid);
    else target[0] = '\0';
    json_str(out, target);
    buf_printf(out, ",\"checks\":{");
    phase4_check_bool(out, "catalog", checks, PH4_CATALOG, 0);
    phase4_check_bool(out, "no_conflicts", checks, PH4_CONFLICT_FREE, 1);
    phase4_check_bool(out, "usb_mounted", checks, PH4_USB_MOUNTED, 1);
    phase4_check_bool(out, "journal_clear", checks, PH4_JOURNAL_CLEAR, 1);
    phase4_check_bool(out, "not_installed_internally", checks, PH4_NOT_INTERNAL, 1);
    phase4_check_bool(out, "paths_safe", checks, PH4_SAFE_PATHS, 1);
    phase4_check_bool(out, "organized", checks, PH4_ORGANIZED, 1);
    phase4_check_bool(out, "metadata_matches", checks, PH4_METADATA_OK, 1);
    phase4_check_bool(out, "database_readable", checks, PH4_DB_READABLE, 1);
    phase4_check_bool(out, "not_registered", checks, PH4_DB_ABSENT, 1);
    buf_printf(out, "}}");
    printf("[safe][launch-check] %s %s: %s (NO mount, NO registration, NO game launch)\n",
           tid, ready ? "PREFLIGHT-PASS" : "BLOCKED", message);
    return 0;
}

/* Web endpoint returns paths for review but NEVER mutates the drive. */
int app_plan_json(const char *tid, buf_t *out) {
    title_t *t = catalog_find(tid);
    title_plan_t plan;
    char why[256];
    auth_clear();
    int ok = supported_usb(t);
    if (!ok) snprintf(why, sizeof(why), "Title is not on a supported mounted USB drive");
    else if (installed_internally(tid)) {
        ok = 0;
        snprintf(why, sizeof(why), "Title is installed internally; no preparation allowed");
    } else if (org_pending(t->root) != 0) {
        ok = 0;
        snprintf(why, sizeof(why), "USB has unfinished organization or unreadable journal; recover first");
    } else if (plan_title(t, &plan, why, sizeof(why))) ok = 0;
    if (ok && pkg_matches_catalog(t, why, sizeof(why))) ok = 0;
    if (ok && (lstat(t->app_path, &g_move_auth.app_stat) ||
               (t->patch_path[0] && lstat(t->patch_path, &g_move_auth.patch_stat)) ||
               auth_nonce())) {
        ok = 0;
        snprintf(why, sizeof(why), "Cannot create secure confirmation token; read-only plan only");
    }
    if (ok) {
        snprintf(g_move_auth.tid, sizeof(g_move_auth.tid), "%s", tid);
        snprintf(g_move_auth.root, sizeof(g_move_auth.root), "%s", t->root);
        snprintf(g_move_auth.app_path, sizeof(g_move_auth.app_path), "%s", t->app_path);
        snprintf(g_move_auth.patch_path, sizeof(g_move_auth.patch_path), "%s", t->patch_path);
        g_move_auth.issued = time(NULL);
    } else auth_clear();
    buf_printf(out, "{\"ok\":%s,\"title_id\":", ok ? "true" : "false");
    json_str(out, tid);
    buf_printf(out, ",\"message\":"); json_str(out, why);
    if (ok) {
        buf_printf(out, ",\"app_source\":"); json_str(out, t->app_path);
        buf_printf(out, ",\"app_destination\":"); json_str(out, plan.app_destination);
        buf_printf(out, ",\"update_source\":"); json_str(out, t->patch_path);
        buf_printf(out, ",\"update_destination\":");
        json_str(out, t->patch_path[0] ? plan.patch_destination : "");
        buf_printf(out, ",\"move_base\":%s,\"move_update\":%s,\"create_folders\":%s,\"read_only\":true,\"confirm_token\":",
                   plan.app_move ? "true" : "false", plan.patch_move ? "true" : "false",
                   (plan.create_library || plan.create_folder) ? "true" : "false");
        json_str(out, g_move_auth.nonce);
    }
    buf_printf(out, "}");
    printf("[safe][plan] %s %s: %s (NO files moved, registration LOCKED)\n",
           tid, ok ? "READY" : "BLOCKED", why);
    return ok ? 0 : -1;
}

/* Explicit USB-only relocation. Authorization expires after 2 minutes and
 * can be consumed exactly once. NO game registration, mounting or SQLite. */
int app_organize(const char *tid, const char *nonce, char *msg, size_t cap) {
    if (!auth_matches(nonce, 0) || strcmp(tid, g_move_auth.tid))
        return plan_error(msg, cap, "Preview expired or confirmation invalid; preview again");
    /* Consume before any reads or filesystem writes. */
    char root[32], old_app[PATH_MAX], old_patch[PATH_MAX];
    struct stat old_app_stat = g_move_auth.app_stat, old_patch_stat = g_move_auth.patch_stat;
    snprintf(root, sizeof(root), "%s", g_move_auth.root);
    snprintf(old_app, sizeof(old_app), "%s", g_move_auth.app_path);
    snprintf(old_patch, sizeof(old_patch), "%s", g_move_auth.patch_path);
    auth_clear();
    title_t *t = catalog_find(tid);
    title_plan_t plan;
    char why[256];
    if (!supported_usb(t) || strcmp(t->root, root) || strcmp(t->app_path, old_app) ||
        strcmp(t->patch_path, old_patch) || installed_internally(tid) ||
        org_pending(t->root) != 0 || plan_title(t, &plan, why, sizeof(why)))
        return plan_error(msg, cap, "USB state changed, game conflicted or journal pending; preview again");
    if (!same_stat(t->app_path, &old_app_stat) ||
        (t->patch_path[0] && !same_stat(t->patch_path, &old_patch_stat)) ||
        pkg_matches_catalog(t, why, sizeof(why)))
        return plan_error(msg, cap, "File changed since preview; rescan and preview again");
    organization_t *req = calloc(1, sizeof(*req));
    if (!req) return plan_error(msg, cap, "Insufficient memory to prepare operation");
    snprintf(req->root, sizeof(req->root), "%s", t->root);
    snprintf(req->tid, sizeof(req->tid), "%s", t->tid);
    snprintf(req->app_source, sizeof(req->app_source), "%s", t->app_path);
    snprintf(req->app_destination, sizeof(req->app_destination), "%s", plan.app_destination);
    snprintf(req->patch_source, sizeof(req->patch_source), "%s", t->patch_path);
    snprintf(req->patch_destination, sizeof(req->patch_destination), "%s", plan.patch_destination);
    int rc = org_apply(req, msg, cap);
    printf("[safe][organize] %s %s: %s (REGISTRATION LOCKED)\n", tid, rc ? "FAILED" : "DONE", msg);
    free(req);
    /* Even on an error, catalog locations may be stale after an interrupted move. */
    g_manual_scan = 1;
    g_next_scan = 0;
    return rc;
}

int app_recovery_json(int usb, buf_t *out) {
    auth_clear();
    if (usb < 0 || usb > 7) return -1;
    char root[32], msg[256];
    snprintf(root, sizeof(root), "/mnt/usb%d", usb);
    if (!is_mountpoint(root)) return -1;
    int state = org_inspect(root, msg, sizeof(msg));
    buf_printf(out, "{\"ok\":%s,\"pending\":%s,\"message\":", state < 0 ? "false" : "true", state == 1 ? "true" : "false");
    json_str(out, msg);
    if (state == 1 && !auth_nonce()) {
        g_move_auth.issued = time(NULL);
        g_move_auth.pending_recovery = 1;
        g_move_auth.usb_number = usb;
        buf_printf(out, ",\"confirm_token\":");
        json_str(out, g_move_auth.nonce);
    }
    buf_printf(out, "}");
    printf("[safe][recovery] %s: %s (READ-ONLY INSPECT)\n", root, msg);
    return state < 0 ? -1 : 0;
}
int app_rollback(int usb, const char *nonce, char *msg, size_t cap) {
    if (usb < 0 || usb > 7 || !auth_matches(nonce, 1) || usb != g_move_auth.usb_number)
        return plan_error(msg, cap, "Recovery authorization expired; inspect again");
    auth_clear();
    char root[32];
    snprintf(root, sizeof(root), "/mnt/usb%d", usb);
    if (!is_mountpoint(root)) return plan_error(msg, cap, "USB is not mounted");
    int rc = org_rollback(root, msg, cap);
    printf("[safe][rollback] %s %s: %s\n", root, rc ? "FAILED" : "DONE", msg);
    g_manual_scan = 1;
    g_next_scan = 0;
    return rc;
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
#if ENABLE_EXPERIMENTAL_REGISTRATION
    sceAppInstUtilInitialize();
#endif
    mkdirs(STATE_DIR, 0777);
    freopen(STATE_DIR "/log.txt", "a", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    /* SAFE PREVIEW: shortcut installation invokes AppInstallAll and writes
     * to PS5 DB even during discovery; never install it automatically. */
    printf("\n======= " VERSION " session start =======\n");
    printf("[safe] shortcut install DISABLED; registration DISABLED; open http://<PS5 IP>:8770\n");
    g_web = web_listen();
    notify(g_web >= 0 ? VERSION " running. ONE-title RO mount probe; registration LOCKED; panel :8770" : VERSION " panel unavailable");
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            if (g_probe.active) {
                char why[256];
                if (probe_release_internal(why, sizeof(why))) {
                    printf("[safe][mount-probe] stop REFUSED: unable to release probe safely: %s\n", why);
                    g_stop = 0; /* stay alive so user can inspect the panel */
                    continue;
                }
            }
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
