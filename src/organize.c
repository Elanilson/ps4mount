/* SPDX-License-Identifier: GPL-3.0-or-later
 * Conservative on-volume journal for one PS4 title's two PKG renames.
 * No PS5 internal paths, installations, nmount, or SQLite writes. */
#include "organize.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ORG_MAGIC "PS4M0051JOURNAL"
#define JOURNAL_FILE "/ps4/.ps4mount-transaction"
#define SIGNATURE_BYTES 65536U
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

typedef struct {
    char source[PATH_MAX], destination[PATH_MAX];
    uint64_t size, signature;
    int present;
} org_pair_t;
typedef struct {
    char magic[16], root[PATH_MAX], tid[10];
    org_pair_t file[2];
    uint64_t checksum;
} org_journal_t;

static int errormsg(char *m, size_t cap, const char *why) {
    if (m && cap) snprintf(m, cap, "%s", why);
    return -1;
}
static uint64_t fnv(uint64_t hash, const void *data, size_t len) {
    const unsigned char *b = data;
    for (size_t i = 0; i < len; i++) { hash ^= b[i]; hash *= FNV_PRIME; }
    return hash;
}
static int valid_path(const char *root, const char *path) {
    size_t n = strlen(root);
    if (!n || strncmp(root, path, n) || path[n] != '/') return 0;
    const char *p = path + n + 1;
    if (!*p) return 0;
    for (;;) {
        const char *q = strchr(p, '/');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (!len || (len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!q) return 1;
        p = q + 1;
    }
}
static int valid_root(const char *root) {
    if (!root || root[0] != '/' || strstr(root, "/../") || strstr(root, "/./") ||
        strstr(root, "//") || strlen(root) >= PATH_MAX - 64) return 0;
    struct stat st;
    return !lstat(root, &st) && S_ISDIR(st.st_mode);
}
/* Every component is inspected with lstat to reject symlink traversal and
 * mount crossings. only_existing=0 allows final filename to be absent. */
static int checked(const char *root, const char *path, int existing, int want_dir) {
    if (!valid_path(root, path)) return -1;
    struct stat r;
    if (lstat(root, &r) || !S_ISDIR(r.st_mode)) return -1;
    size_t n = strlen(root);
    char p[PATH_MAX];
    for (const char *c = path + n + 1; ; c++) {
        if (*c && *c != '/') continue;
        size_t k = (size_t)(c - path);
        if (k >= sizeof(p)) return -1;
        memcpy(p, path, k); p[k] = 0;
        struct stat s;
        if (lstat(p, &s)) {
            if (!*c && !existing && errno == ENOENT) return 0;
            return -1;
        }
        if (s.st_dev != r.st_dev) return -1;
        if (!*c) return (want_dir ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode)) ? 0 : -1;
        if (!S_ISDIR(s.st_mode)) return -1;
    }
}
static int journal_path(const char *root, char *out, size_t cap) {
    return snprintf(out, cap, "%s%s", root, JOURNAL_FILE) < (int)cap ? 0 : -1;
}
static int file_signature(const char *path, uint64_t *size, uint64_t *signature) {
    struct stat before, after;
    if (lstat(path, &before) || !S_ISREG(before.st_mode) || before.st_size < 0) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &after) || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size) { close(fd); return -1; }
    unsigned char b[8192];
    uint64_t h = FNV_OFFSET, len = (uint64_t)before.st_size;
    /* Read start and end, not the entire potentially 100-GB file. */
    for (int side = 0; side < 2; side++) {
        uint64_t portion = len < SIGNATURE_BYTES ? len : SIGNATURE_BYTES;
        off_t pos = side ? (off_t)(len - portion) : 0;
        if (lseek(fd, pos, SEEK_SET) != pos) { close(fd); return -1; }
        while (portion) {
            size_t want = portion < sizeof(b) ? (size_t)portion : sizeof(b);
            ssize_t n = read(fd, b, want);
            if (n <= 0) { close(fd); return -1; }
            h = fnv(h, b, (size_t)n); portion -= (uint64_t)n;
        }
    }
    if (fstat(fd, &after) || after.st_size != before.st_size || after.st_mtime != before.st_mtime ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino) { close(fd); return -1; }
    if (close(fd)) return -1;
    *size = len; *signature = fnv(h, &len, sizeof(len));
    return 0;
}
static int same_file(const char *path, const org_pair_t *f) {
    uint64_t n, h;
    return !file_signature(path, &n, &h) && n == f->size && h == f->signature;
}
static int exists_any(const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}
static int make_folder(const char *root, const char *path) {
    int ex = exists_any(path);
    if (ex == 1) return checked(root, path, 1, 1);
    if (ex != 0 || mkdir(path, 0755)) return -1;
    return checked(root, path, 1, 1);
}
static int all_paths_valid(const org_journal_t *j) {
    if (!valid_root(j->root)) return -1;
    if (strncmp(j->magic, ORG_MAGIC, sizeof(j->magic)) || j->magic[15] != 0 ||
        strlen(j->tid) != 9 || strncmp(j->tid, "CUSA", 4)) return -1;
    for (int k = 4; k < 9; k++) if (j->tid[k] < '0' || j->tid[k] > '9') return -1;
    for (int i = 0; i < 2; i++) {
        const org_pair_t *f = &j->file[i];
        if (!f->present) continue;
        char prefix[PATH_MAX];
        if (snprintf(prefix, sizeof(prefix), "%s/ps4/", j->root) >= (int)sizeof(prefix) ||
            strncmp(f->destination, prefix, strlen(prefix)) ||
            !valid_path(j->root, f->source) || !valid_path(j->root, f->destination)) return -1;
        const char *tail = strrchr(f->destination, '/');
        if (!tail || strcmp(tail, i ? "/patch.pkg" : "/app.pkg")) return -1;
        /* Must not ever touch a file outside the current title's destination. */
        if (strncmp(f->destination + strlen(prefix), j->tid, 9)) return -1;
    }
    return j->file[0].present ? 0 : -1;
}
static uint64_t journal_checksum(const org_journal_t *j) {
    return fnv(FNV_OFFSET, j, offsetof(org_journal_t, checksum));
}
static int load_journal(const char *root, org_journal_t *j) {
    char jp[PATH_MAX];
    if (!valid_root(root) || journal_path(root, jp, sizeof(jp)) ||
        checked(root, jp, 1, 0)) return -1;
    int fd = open(jp, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t pos = 0;
    unsigned char *p = (unsigned char *)j;
    while (pos < sizeof(*j)) {
        ssize_t n = read(fd, p + pos, sizeof(*j) - pos);
        if (n <= 0) { close(fd); return -1; }
        pos += (size_t)n;
    }
    unsigned char extra;
    int too_large = read(fd, &extra, 1) != 0;
    close(fd);
    if (too_large || memcmp(j->magic, ORG_MAGIC, sizeof(j->magic)) ||
        j->checksum != journal_checksum(j) || strcmp(j->root, root) ||
        all_paths_valid(j)) return -1;
    return 0;
}
int org_pending(const char *root) {
    char jp[PATH_MAX];
    if (!valid_root(root) || journal_path(root, jp, sizeof(jp))) return -1;
    return exists_any(jp);
}
int org_inspect(const char *root, char *msg, size_t cap) {
    int state = org_pending(root);
    if (state == 0) { snprintf(msg, cap, "No interrupted move journal on this USB drive"); return 0; }
    if (state != 1) return errormsg(msg, cap, "Cannot inspect USB recovery journal");
    org_journal_t *j = calloc(1, sizeof(*j));
    if (!j) return errormsg(msg, cap, "Not enough memory to inspect journal");
    if (load_journal(root, j)) { free(j); return errormsg(msg, cap, "Journal invalid; manual inspection required. No automatic changes."); }
    snprintf(msg, cap, "Interrupted move for %s. Recovery required; no new moves allowed.", j->tid);
    free(j); return 1;
}
static int write_journal(const char *root, org_journal_t *j) {
    char jp[PATH_MAX];
    if (journal_path(root, jp, sizeof(jp))) return -1;
    j->checksum = journal_checksum(j);
    int fd = open(jp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n < sizeof(*j)) {
        ssize_t r = write(fd, (char *)j + n, sizeof(*j) - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return -1; } /* leave journal to block further moves */
        n += (size_t)r;
    }
    int syncerr = fsync(fd), closeerr = close(fd);
    if (!syncerr && !closeerr) sync(); /* journal must precede package rename */
    return syncerr || closeerr ? -1 : 0;
}
/* In the absence of a no-replace rename on every supported PS5 firmware, we
 * check destination immediately before rename. Avoid concurrent USB writers. */
static int perform_move(const char *root, const org_pair_t *f) {
    if (!f->present || !strcmp(f->source, f->destination)) return 0;
    if (checked(root, f->source, 1, 0) || checked(root, f->destination, 0, 0) ||
        exists_any(f->destination) != 0 || !same_file(f->source, f)) return -1;
    if (rename(f->source, f->destination)) return -1;
    return same_file(f->destination, f) ? 0 : -1;
}
/* Inspect BOTH paths before attempting rollback: only one must hold the
 * expected package. Never overwrite user data or invent a destination. */
int org_rollback(const char *root, char *msg, size_t cap) {
    org_journal_t *j = calloc(1, sizeof(*j));
    if (!j) return errormsg(msg, cap, "Out of memory");
    if (load_journal(root, j)) { free(j); return errormsg(msg, cap, "Invalid or absent journal; rollback refused"); }
    for (int i = 0; i < 2; i++) {
        org_pair_t *f = &j->file[i];
        if (!f->present) continue;
        if (!strcmp(f->source, f->destination)) {
            if (checked(root, f->source, 1, 0) || !same_file(f->source, f)) {
                free(j); return errormsg(msg, cap, "Already placed PKG changed; rollback refused");
            }
            continue;
        }
        int s = exists_any(f->source), d = exists_any(f->destination);
        if (s < 0 || d < 0 || (s && d) || (!s && !d) ||
            (s && (!same_file(f->source, f) || checked(root, f->source, 1, 0))) ||
            (d && (!same_file(f->destination, f) || checked(root, f->destination, 1, 0)))) {
            free(j); return errormsg(msg, cap, "Rollback stopped: missing/changed/duplicate file. USB journal preserved");
        }
    }
    for (int i = 1; i >= 0; i--) {
        org_pair_t *f = &j->file[i];
        if (!f->present || !strcmp(f->source, f->destination) || exists_any(f->source) == 1) continue;
        if (checked(root, f->source, 0, 0) || checked(root, f->destination, 1, 0) ||
            exists_any(f->source) != 0 || !same_file(f->destination, f) ||
            rename(f->destination, f->source)) {
            free(j); return errormsg(msg, cap, "Rollback incomplete: journal preserved. Do not disconnect USB");
        }
    }
    sync(); /* flush reverse renames before destroying recovery evidence */
    char jp[PATH_MAX];
    if (journal_path(root, jp, sizeof(jp)) || unlink(jp)) {
        free(j); return errormsg(msg, cap, "Files reverted, but journal removal failed");
    }
    sync();
    snprintf(msg, cap, "%s: files restored to original paths; empty new folders can remain", j->tid);
    free(j); return 0;
}
int org_apply(const organization_t *r, char *msg, size_t cap) {
    if (!r || !valid_root(r->root) || strlen(r->tid) != 9 ||
        strncmp(r->tid, "CUSA", 4)) return errormsg(msg, cap, "Invalid USB root or title id");
    int pending = org_pending(r->root);
    if (pending) return errormsg(msg, cap, "USB has recovery journal or cannot be inspected: move blocked");
    org_journal_t *j = calloc(1, sizeof(*j));
    if (!j) return errormsg(msg, cap, "Out of memory");
    memcpy(j->magic, ORG_MAGIC, sizeof(j->magic));
    snprintf(j->root, sizeof(j->root), "%s", r->root);
    snprintf(j->tid, sizeof(j->tid), "%s", r->tid);
    const char *sources[2] = {r->app_source, r->patch_source};
    const char *dests[2] = {r->app_destination, r->patch_destination};
    for (int i = 0; i < 2; i++) {
        org_pair_t *f = &j->file[i];
        if (!sources[i][0]) continue;
        f->present = 1;
        snprintf(f->source, sizeof(f->source), "%s", sources[i]);
        snprintf(f->destination, sizeof(f->destination), "%s", dests[i]);
        if (checked(r->root, f->source, 1, 0) ||
            file_signature(f->source, &f->size, &f->signature)) {
            free(j); return errormsg(msg, cap, "PKG source missing, unsafe, or changed");
        }
    }
    if (all_paths_valid(j)) { free(j); return errormsg(msg, cap, "Invalid destination, refusing USB write"); }
    /* Create the two directories only after validating source files and
       before persisting the journal. New empty folders are harmless on failure. */
    char library[PATH_MAX], title_dir[PATH_MAX];
    snprintf(library, sizeof(library), "%s/ps4", r->root);
    snprintf(title_dir, sizeof(title_dir), "%s", r->app_destination);
    char *last = strrchr(title_dir, '/');
    if (!last) { free(j); return errormsg(msg, cap, "Destination missing parent"); }
    *last = 0;
    if (make_folder(r->root, library) || make_folder(r->root, title_dir) ||
        checked(r->root, j->file[0].destination, 0, 0) ||
        (j->file[1].present && checked(r->root, j->file[1].destination, 0, 0))) {
        free(j); return errormsg(msg, cap, "Destination folder unsafe; no PKGs moved");
    }
    for (int i = 0; i < 2; i++) {
        org_pair_t *f = &j->file[i];
        if (f->present && strcmp(f->source, f->destination) && exists_any(f->destination) != 0) {
            free(j); return errormsg(msg, cap, "Destination occupied; no PKGs moved");
        }
    }
    if (write_journal(r->root, j)) {
        free(j); return errormsg(msg, cap, "Journal creation/sync failed. Resolve any journal before retrying; NO PKGs moved");
    }
    int ok = !perform_move(r->root, &j->file[0]) && !perform_move(r->root, &j->file[1]);
    free(j);
    if (!ok) {
        char why[256];
        if (org_rollback(r->root, why, sizeof(why)) == 0)
            return errormsg(msg, cap, "Move failed; original PKGs restored (check paths and rescan)");
        return errormsg(msg, cap, "Move/rollback FAILED. USB journal remains: manually recover before any new move");
    }
    sync(); /* flush both completed renames before removing journal */
    char jp[PATH_MAX];
    if (journal_path(r->root, jp, sizeof(jp)) || unlink(jp))
        return errormsg(msg, cap, "Files moved but recovery journal remains; check USB before proceeding");
    sync();
    snprintf(msg, cap, "%s: PKGs organized on USB. NO registration/mount performed", r->tid);
    return 0;
}
