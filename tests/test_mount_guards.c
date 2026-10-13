/* Host-only test: mount functions must not touch foreign mounts. */
#define BATCH_FILE "/tmp/ps4mount-phase4h-host-safety.state"
#define GROUP_FILE "/tmp/ps4mount-phase4m-host-group.state"
#define main ps4mount_console_main
#include "../src/main.c"
#undef main
#include <assert.h>

/* Simulated PS5 API/DB: unknown DB status must fail every group barrier. */
int db_registered_status(const char *tid) { (void)tid; return -1; }
int org_pending(const char *root) { (void)root; return 0; }
int sceKernelSendNotificationRequest(int u, notify_request_t *r, size_t z, int p) {
    (void)u; (void)r; (void)z; (void)p; return 0;
}
static int unmount_calls, mount_calls;
static int test_base_only_mode, patch_statfs_reads;
/* Simulate a PS5 with a known RO app mount and an UNRELATED foreign layer at
 * /user/patch/CUSA06886, without touching real host /user paths. */
int lstat(const char *p, struct stat *st) {
    if (test_base_only_mode &&
        (!strcmp(p, "/user/app/CUSA06886") ||
         !strcmp(p, "/user/patch/CUSA06886"))) {
        memset(st, 0, sizeof(*st)); st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    return fstatat(AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW);
}
int statfs(const char *p, struct statfs *sf) {
    memset(sf, 0, sizeof(*sf));
    snprintf(sf->f_mntonname, sizeof(sf->f_mntonname), "%s", p);
    if (test_base_only_mode && !strcmp(p, "/user/app/CUSA06886")) {
        snprintf(sf->f_fstypename, sizeof(sf->f_fstypename), "nullfs");
        snprintf(sf->f_mntfromname, sizeof(sf->f_mntfromname), "/mnt/usb0/CUSA06886");
        sf->f_flags = MNT_RDONLY;
    } else {
        if (test_base_only_mode && !strcmp(p, "/user/patch/CUSA06886"))
            patch_statfs_reads++;
        snprintf(sf->f_fstypename, sizeof(sf->f_fstypename), "foreignfs");
    }
    return 0;
}
int nmount(void *iov, unsigned int niov, int flags) {
    (void)iov; (void)niov; (void)flags;
    mount_calls++; return -1;
}
int unmount(const char *path, int flags) {
    (void)path; (void)flags;
    unmount_calls++; return test_base_only_mode ? 0 : -1;
}
int getfsstat(struct statfs *buf, long size, int flags) {
    (void)size; (void)flags;
    if (test_base_only_mode) {
        if (buf) {
            memset(buf, 0, sizeof(*buf));
            snprintf(buf->f_mntonname, sizeof(buf->f_mntonname), "/mnt/other");
        }
        return 1; /* no matching running sandbox */
    }
    return -1;  /* unreadable running-state: must be fail-closed */
}
int main(void) {
    char template[] = "/tmp/ps4mount-mount-XXXXXX";
    char *root = mkdtemp(template); assert(root);
    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/CUSA01623", root);
    assert(mkdir(target, 0700) == 0);
    assert(top_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    assert(mount_dir("/mnt/usb0/ps4/CUSA01623", target, 1) == -1);
    assert(unmount_owned_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    assert(unmount_calls == 0 && mount_calls == 0);
    assert(title_running("CUSA01623") == 1);
    /* Phase 4B: refuse unmounts not attributable to our own source. */
    assert(probe_empty_dir(target) == 1);
    char inside[PATH_MAX];
    snprintf(inside, sizeof(inside), "%s/preserve.txt", target);
    FILE *f = fopen(inside, "wb"); assert(f);
    assert(fputs("do not obscure", f) >= 0); fclose(f);
    assert(probe_empty_dir(target) == 0);
    assert(unlink(inside) == 0);
    assert(probe_layer(target, "/mnt/usb0/ps4/CUSA01623") == -1);
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", root);
    assert(probe_layer(missing, "/mnt/usb0/ps4/CUSA01623") == 0);
    assert(unmount_calls == 0 && mount_calls == 0);
    printf("PASS: Phase 4B marks foreign mountpoint and nonempty destination as unsafe\n");

    /* Two distinct attempts cannot reuse or guess a confirmation. */
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    char token[33];
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    assert(!probe_auth_consume("CUSA01623", "mount", token, NULL, NULL, NULL, NULL));
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    assert(probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(probe_auth_generate("CUSA01623", "release", NULL) == 0);
    snprintf(token, sizeof(token), "%s", g_probe_auth.token);
    g_probe_auth.issued -= 121;
    assert(!probe_auth_consume("CUSA01623", "release", token, NULL, NULL, NULL, NULL));
    assert(mount_calls == 0 && unmount_calls == 0);
    printf("PASS: Phase 4B rejects wrong-mode, expired, replayed, and invalid tokens\n");
    /* Phase 4H: backend queue state is independent of the browser. */
    assert(!batch_running());
    g_batch.phase = BATCH_PREPARE;
    assert(batch_running());
    g_batch.phase = BATCH_VERIFY;
    assert(batch_running());
    g_batch.phase = BATCH_FAILED;
    assert(!batch_running());
    g_batch.phase = BATCH_INTERRUPTED;
    assert(!batch_running());
    char good[] = "{\"can_mount\":true,\"confirm_token\":\"0123456789abcdef0123456789abcdef\"}";
    char bad[] = "{\"can_mount\":false,\"confirm_token\":\"0123456789abcdef0123456789abcdef\"}";
    buf_t preview = { .data = (uint8_t*)good, .len = strlen(good) };
    char batch_token[33];
    assert(!batch_get_token(&preview, "\"can_mount\":true", batch_token));
    assert(!strcmp(batch_token, "0123456789abcdef0123456789abcdef"));
    preview.data = (uint8_t*)bad; preview.len = strlen(bad);
    assert(batch_get_token(&preview, "\"can_mount\":true", batch_token));
    puts("PASS: Phase 4H daemon phase boundaries and verified internal action tokens");
    /* A crashed daemon cannot retry a previously started phase on next boot. */
    unlink(BATCH_FILE);
    memset(&g_batch,0,sizeof(g_batch));
    g_batch.phase=BATCH_RELEASE;
    g_batch.count=1;
    snprintf(g_batch.titles[0],10,"CUSA03323");
    snprintf(g_batch.message,sizeof(g_batch.message),"last step may have completed");
    assert(batch_store()==0);
    memset(&g_batch,0,sizeof(g_batch));
    batch_restore();
    assert(g_batch.phase==BATCH_INTERRUPTED);
    assert(!batch_running());
    assert(strstr(g_batch.message,"No automatic replay"));
    unlink(BATCH_FILE);
    puts("PASS: Phase 4H restores unfinished work as INTERRUPTED without any automatic replay");

    assert(group_next_count(12,0)==5 && group_next_count(12,5)==5 &&
           group_next_count(12,10)==2 && group_next_count(12,12)==0);
    puts("PASS: Phase 4M groups exactly 5 + 5 + 2 for 12 titles");
        /* Manual group job: persisted full snapshot may NEVER auto-resume on restart. */
    unlink(GROUP_FILE);memset(&g_job,0,sizeof(g_job));
    g_job.phase=JOB_RUNNING;g_job.total=12;g_job.done=5;
    g_job.verified=5;g_job.current_group=1;
    for(int i=0;i<12;i++)snprintf(g_job.titles[i],10,"CUSA%05d",i);
    snprintf(g_job.message,sizeof(g_job.message),"group two prepared");
    assert(job_store()==0);
    memset(&g_job,0,sizeof(g_job));job_restore();
    assert(g_job.phase==JOB_INTERRUPTED);
    assert(g_job.total==12 && g_job.done==5 && g_job.verified==5);
    assert(strstr(g_job.message,"NO automatic replay"));
    assert(!job_active());
    /* Even if a restart finds the old group already completed, a pending full
     * queue cannot silently run the next group. */
    unlink(GROUP_FILE);memset(&g_job,0,sizeof(g_job));
    g_job.phase=JOB_WAITING;g_job.total=12;g_job.done=5;g_job.verified=5;
    g_job.current_group=1;g_job.wait_until=time(NULL)+10;
    for(int i=0;i<12;i++)snprintf(g_job.titles[i],10,"CUSA%05d",i);
    assert(job_store()==0);memset(&g_job,0,sizeof(g_job));job_restore();
    assert(g_job.phase==JOB_INTERRUPTED && !job_active());
    unlink(GROUP_FILE);
    puts("PASS: Phase 4M full-group snapshots RUNNING/WAITING never replay after restart");
    /* Authorization is scoped to exactly one CUSA and BATCH_REGISTER step. */
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    g_job.phase=JOB_RUNNING;g_job.total=6;
    snprintf(g_job.titles[0],10,"CUSA11111");
    g_batch.phase=BATCH_REGISTER;g_batch.count=1;
    snprintf(g_batch.titles[0],10,"CUSA11111");
    assert(batch_authorized_title("CUSA11111"));
    assert(!batch_authorized_title("CUSA22222"));
    g_batch.phase=BATCH_VERIFY;
    assert(!batch_authorized_title("CUSA11111"));
    g_job.phase=JOB_FAILED;
    assert(!batch_authorized_title("CUSA11111"));
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    puts("PASS: Phase 4M per-CUSA authorization requires active group register step");

    /* A partly successful group is terminal; never advance to group #2. */
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    g_job.phase=JOB_RUNNING;g_job.total=7;g_job.current_group=0;
    g_batch.phase=BATCH_DONE;g_batch.count=5;g_batch.index=5;g_batch.registered=4;
    group_tick();
    assert(g_job.phase==JOB_FAILED && g_job.done==0 && !job_active());
    assert(strstr(g_job.message,"Group not completely verified"));
    unlink(GROUP_FILE);
    /* Even a nominally completed group with unconfirmed DB records fails. */
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    g_job.phase=JOB_RUNNING;g_job.total=1;
    g_batch.phase=BATCH_DONE;g_batch.count=1;g_batch.index=1;g_batch.registered=1;
    snprintf(g_batch.titles[0],10,"CUSA11111");
    group_tick();
    assert(g_job.phase==JOB_FAILED && g_job.done==0);
    assert(strstr(g_job.message,"barrier FAILED"));
    unlink(GROUP_FILE);
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    puts("PASS: Phase 4M incomplete and DB-uncertain groups NEVER start next group");
    /* A title that disappears from app.db during the cooldown also blocks
     * the start of the following group. */
    g_job.phase=JOB_WAITING;g_job.total=6;g_job.done=5;g_job.verified=5;
    g_job.current_group=1;g_job.wait_until=time(NULL)-1;
    g_batch.phase=BATCH_DONE;g_batch.count=5;g_batch.index=5;g_batch.registered=5;
    for(int i=0;i<5;i++)snprintf(g_batch.titles[i],10,"CUSA%05d",i);
    group_tick();
    assert(g_job.phase==JOB_FAILED && g_job.done==5);
    assert(strstr(g_job.message,"Previous group changed during pause"));
    unlink(GROUP_FILE);
    memset(&g_job,0,sizeof(g_job));memset(&g_batch,0,sizeof(g_batch));
    puts("PASS: Phase 4M rechecks all prior titles AFTER cooldown before advancing");


    /* Phase 4I: no batch before USB discovery; interrupted state blocks
     * unattended writes permanently for the current ELF session. */
    assert(AUTO_BOOT_BATCH_LIMIT == 0); /* disabled after real PS5 mount conflict */
    g_auto_boot_attempted = 0;
    g_title_count = 0;
    assert(batch_autoboot_gate() == 0);
    g_title_count = 1;
    g_batch.phase = BATCH_INTERRUPTED;
    assert(batch_autoboot_gate() == -1); /* crash state blocks */
    g_batch.phase = BATCH_IDLE;
    assert(batch_autoboot_gate() == 1); /* eligible to inspect */
    g_auto_boot_attempted = 1;
    assert(batch_autoboot_gate() == 0); /* one-shot */
    g_auto_boot_attempted = 0;
    g_managed[0].used = 1;
    g_managed[0].running = 1;
    assert(batch_autoboot_gate() == -2); /* active game */
    puts("PASS: Phase 4J retains crash/active-game gating, auto-boot disabled");
    /* Phase 4K: a BASE-ONLY CUSA never even INSPECTS the foreign patch layer. */
    test_base_only_mode = 1;
    patch_statfs_reads = 0;
    assert(mount_paths("/mnt/usb0/CUSA06886", "CUSA06886", 0, 0) == 0);
    assert(patch_statfs_reads == 0);
    /* A title WITH a patch still refuses that very same foreign layer. */
    assert(mount_paths("/mnt/usb0/CUSA06886", "CUSA06886", 1, 0) == -1);
    assert(patch_statfs_reads > 0);
    patch_statfs_reads = 0;
    managed_t *m = &g_managed[0];
    memset(m, 0, sizeof(*m)); m->used=1; m->has_patch=0;
    snprintf(m->tid, sizeof(m->tid), "CUSA06886");
    snprintf(m->dir, sizeof(m->dir), "/mnt/usb0/CUSA06886");
    unmount_title("CUSA06886");
    assert(patch_statfs_reads == 0);
    patch_statfs_reads = 0;
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.active=1; g_probe.has_patch=0;
    snprintf(g_probe.tid, sizeof(g_probe.tid), "CUSA06886");
    snprintf(g_probe.dir, sizeof(g_probe.dir), "/mnt/usb0/CUSA06886");
    char phase4k_msg[256];
    assert(probe_release_internal(phase4k_msg,sizeof(phase4k_msg))==0);
    assert(patch_statfs_reads==0);
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.active=1; g_probe.has_patch=1;
    snprintf(g_probe.tid, sizeof(g_probe.tid), "CUSA06886");
    snprintf(g_probe.dir, sizeof(g_probe.dir), "/mnt/usb0/CUSA06886");
    int count_before = unmount_calls;
    assert(probe_release_internal(phase4k_msg,sizeof(phase4k_msg))==-1);
    assert(patch_statfs_reads>0);
    assert(unmount_calls==count_before); /* foreign layer blocks ANY unmount */
    assert(AUTO_BOOT_BATCH_LIMIT==0 && !strcmp(ISOLATED_TEST_TID,"CUSA06886"));
    g_batch.phase=BATCH_FAILED;
    assert(prior_batch_unresolved());
    g_batch.phase=BATCH_IDLE;
    assert(!prior_batch_unresolved());
    puts("PASS: Phase 4K base-only ignores foreign patch; updated games fail closed; prior failure remains locked");
    test_base_only_mode = 0;
    assert(rmdir(target) == 0);
    assert(rmdir(root) == 0);
    puts("PASS: foreign mounts left untouched and unreadable running-state blocks mutations");
    return 0;
}
