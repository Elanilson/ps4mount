/* SPDX-License-Identifier: GPL-3.0-or-later
 * Does not touch mounts or USB storage. The system-wide AppInstallAll scanner is
 * undocumented; hiding PKG symlinks does NOT guarantee title-scoped behavior. */
#include "isolation.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define PREFIX "__ps4mount_hidden__"
#define MARKER "isolation_target"

typedef struct {
    char hold[PATH_MAX], state[PATH_MAX], target[10];
    int restore, preflight;
    isolation_report_t *report;
} ctx_t;

static int is_cusa(const char *s) {
    if (strlen(s) != 9 || memcmp(s, "CUSA", 4)) return 0;
    for (int i = 4; i < 9; ++i) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}
static int directory(const char *p) {
    struct stat st;
    return !lstat(p, &st) && S_ISDIR(st.st_mode);
}
static int checked_path(char *dst, size_t n, const char *base, const char *name) {
    int w = snprintf(dst, n, "%s/%s", base, name);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}
static int marker_path(const char *state, char *out, size_t n) {
    return checked_path(out, n, state, MARKER);
}
static int get_target(const char *state, char target[10]) {
    char path[PATH_MAX], buf[16] = {0};
    if (marker_path(state, path, sizeof(path))) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != 9 || !is_cusa(buf)) return -1;
    memcpy(target, buf, 10);
    return 1;
}
static int save_target(const char *state, const char *target) {
    char path[PATH_MAX], tmp[PATH_MAX];
    if (marker_path(state, path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, target, 9) == 9 && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}
static int clear_target(const char *state) {
    char path[PATH_MAX];
    if (marker_path(state, path, sizeof(path))) return -1;
    return !unlink(path) || errno == ENOENT ? 0 : -1;
}
/* Only a symlink directly owned by PS4Mount is eligible. No attempt is made
 * to look through /user, which would traverse a live mountpoint. */
static int visit_link(ctx_t *c, const char *folder, const char *name, const char *owner) {
    if (!is_cusa(owner)) return 0;
    char normal[PATH_MAX], hidden[PATH_MAX], hidden_name[96];
    snprintf(hidden_name, sizeof(hidden_name), PREFIX "%s", name);
    if (checked_path(normal, sizeof(normal), folder, name) ||
        checked_path(hidden, sizeof(hidden), folder, hidden_name)) return -1;
    struct stat a, b;
    int have_a = !lstat(normal, &a), have_b = !lstat(hidden, &b);
    if (c->restore) {
        if (!have_b) return 0;
        if (!S_ISLNK(b.st_mode)) { c->report->conflicts++; return -1; }
        c->report->examined++;
        c->report->already_hidden++;
        if (have_a) { c->report->conflicts++; return -1; }
        if (!c->preflight) {
            if (rename(hidden, normal)) { c->report->failures++; return -1; }
            c->report->changed++;
            printf("[isolation] RESTORED %s\n", normal);
        }
        return 0;
    }
    if (!strcmp(owner, c->target)) return 0;
    if (have_b) { c->report->conflicts++; return -1; }
    if (!have_a || !S_ISLNK(a.st_mode)) return 0;
    c->report->examined++;
    if (!c->preflight) {
        if (rename(normal, hidden)) { c->report->failures++; return -1; }
        c->report->changed++;
        printf("[isolation] HIDDEN %s -> %s\n", normal, hidden_name);
    }
    return 0;
}
static int walk(ctx_t *c) {
    int errors = 0;
    const char *kinds[3] = {"app", "patch", "addcont"};
    const char *names[3] = {"app.pkg", "patch.pkg", "ac.pkg"};
    for (int k = 0; k < 3; ++k) {
        char kind_path[PATH_MAX];
        if (checked_path(kind_path, sizeof(kind_path), c->hold, kinds[k])) return -1;
        DIR *d = opendir(kind_path);
        if (!d) { if (errno != ENOENT) errors++; continue; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!is_cusa(e->d_name)) continue;
            char group[PATH_MAX];
            if (checked_path(group, sizeof(group), kind_path, e->d_name) || !directory(group)) continue;
            if (k < 2) {
                errors += visit_link(c, group, names[k], e->d_name) != 0;
                continue;
            }
            DIR *dc = opendir(group);
            if (!dc) { errors++; continue; }
            struct dirent *label;
            while ((label = readdir(dc))) {
                if (!strcmp(label->d_name, ".") || !strcmp(label->d_name, "..")) continue;
                char folder[PATH_MAX];
                if (checked_path(folder, sizeof(folder), group, label->d_name) || !directory(folder)) continue;
                errors += visit_link(c, folder, names[k], e->d_name) != 0;
            }
            closedir(dc);
        }
        closedir(d);
    }
    return errors ? -1 : 0;
}
int isolation_status(const char *hold, const char *state, isolation_report_t *out) {
    memset(out, 0, sizeof(*out));
    int marker = get_target(state, out->target);
    if (marker < 0) return -1;
    ctx_t c = { .restore = 1, .preflight = 1, .report = out };
    snprintf(c.hold, sizeof(c.hold), "%s", hold);
    snprintf(c.state, sizeof(c.state), "%s", state);
    int rc = walk(&c);
    /* marker 0 but hidden links >0: interrupted rename, restore is still possible. */
    return rc ? -1 : marker;
}
int isolation_restore(const char *hold, const char *state, isolation_report_t *out) {
    memset(out, 0, sizeof(*out));
    int marker = get_target(state, out->target);
    if (marker < 0) return -1;
    ctx_t c = { .restore = 1, .preflight = 1, .report = out };
    snprintf(c.hold, sizeof(c.hold), "%s", hold);
    snprintf(c.state, sizeof(c.state), "%s", state);
    if (walk(&c)) return -1;  /* do not partially restore if a conflict exists */
    c.preflight = 0;
    out->examined = out->already_hidden = 0;
    if (walk(&c)) return -1;
    return clear_target(state);
}
int isolation_enable(const char *hold, const char *state, const char *target,
                     isolation_report_t *out) {
    memset(out, 0, sizeof(*out));
    if (!is_cusa(target)) return -1;
    int marker = get_target(state, out->target);
    if (marker != 0) return -1; /* one isolation at a time; restore first */
    /* Refuse leftover hidden links from an interrupted session. */
    isolation_report_t stale;
    if (isolation_status(hold, state, &stale) < 0 || stale.already_hidden) return -1;
    ctx_t c = { .restore = 0, .preflight = 1, .report = out };
    snprintf(c.hold, sizeof(c.hold), "%s", hold);
    snprintf(c.state, sizeof(c.state), "%s", state);
    snprintf(c.target, sizeof(c.target), "%s", target);
    if (walk(&c) || out->examined == 0) return -1;
    if (save_target(state, target)) return -1; /* write-ahead recovery marker */
    c.preflight = 0;
    out->examined = 0;
    if (walk(&c)) {
        isolation_report_t rollback;
        if (isolation_restore(hold, state, &rollback))
            printf("[isolation] ERROR: rollback incomplete; use RESTORE button\n");
        return -1;
    }
    snprintf(out->target, sizeof(out->target), "%s", target);
    return 0;
}
