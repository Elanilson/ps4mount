/* Host-only integration tests for nested pkg folder discovery. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "scan_walk.h"

static int found, root_pkgs, nested_app, nested_patch, nested_dlc, custom_dir;
static int probed_dirs, probed_managed;

static void make_dir(const char *p) { assert(!mkdir(p, 0700)); }
static void touch(const char *p) {
    FILE *f = fopen(p, "w");
    assert(f);
    fputs("test", f);
    fclose(f);
}
static int visit(const char *p, void *arg) {
    const char *root = arg;
    found++;
    if (!strcmp(root, p)) root_pkgs++;
    if (strstr(p, "/Collection/Game A")) nested_app++;
    if (strstr(p, "/Collection/Game B/Updates")) nested_patch++;
    if (strstr(p, "/Collection/Game B/DLCs")) nested_dlc++;
    if (strstr(p, "/ps4/Other Games")) custom_dir++;
    /* A canonical managed game's pkg must not be searched again. */
    assert(!strstr(p, "/ps4/CUSA00001"));
    return 1;
}
static int probe(const char *p, void *arg) { (void)p; (void)arg; return 1; }
static void check_progress(const char *p, void *arg) {
    (void)arg;
    probed_dirs++;
    if (strstr(p, "/ps4/CUSA00001")) probed_managed++;
}
int main(void) {
    char template[] = "/tmp/ps4mount-walk-XXXXXX";
    char *root = mkdtemp(template);
    assert(root);
    char p[2048], lib[2048], nested[2048];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    make_dir(lib);
    snprintf(p, sizeof(p), "%s/ps4/CUSA00001", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/ps4/CUSA00001/app.pkg", root); touch(p);
    snprintf(p, sizeof(p), "%s/ps4/CUSA00001/addcont", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/ps4/Other Games", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/ps4/Other Games/extra.pkg", root); touch(p);
    snprintf(p, sizeof(p), "%s/Collection", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Collection/Game A", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Collection/Game A/app.pkg", root); touch(p);
    snprintf(p, sizeof(p), "%s/Collection/Game B", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Collection/Game B/Updates", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Collection/Game B/Updates/PATCH.PKG", root); touch(p);
    snprintf(p, sizeof(p), "%s/Collection/Game B/DLCs", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/Collection/Game B/DLCs/ac.pkg", root); touch(p);
    snprintf(p, sizeof(p), "%s/root.pkg", root); touch(p);
    snprintf(p, sizeof(p), "%s/empty", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/System Volume Information", root); make_dir(p);
    snprintf(p, sizeof(p), "%s/System Volume Information/hidden.pkg", root); touch(p);
    /* A symlink to a parent must not cause an infinite walk. */
    snprintf(p, sizeof(p), "%s/Collection/Game B/loop", root); assert(!symlink(root, p));
    /* A directory with a .pkg suffix is not a package. */
    snprintf(p, sizeof(p), "%s/empty/directory.pkg", root); make_dir(p);
    assert(walk_pkg_dirs_progress(root, lib, visit, root, 0, check_progress, NULL) == 5);
    assert(probed_dirs >= 7 && probed_managed == 0);
    assert(found == 5 && root_pkgs == 1 && nested_app == 1 &&
           nested_patch == 1 && nested_dlc == 1 && custom_dir == 1);
    assert(walk_pkg_dirs(root, lib, probe, NULL, 1) == 1);
    snprintf(nested, sizeof(nested), "%s/Collection/Game B", root);
    assert(walk_pkg_dirs(nested, lib, probe, NULL, 0) == 2);
    assert(walk_pkg_dirs("/does-not-exist", lib, probe, NULL, 0) == 0);
    /* Cleanup test fixture without touching anything outside our own temp tree. */
    snprintf(p, sizeof(p), "rm -rf '%s'", root);
    assert(system(p) == 0);
    puts("Recursive PKG discovery + live folder callback: OK (root, nested DLC/patch, exclusion, symlink)");
    return 0;
}
