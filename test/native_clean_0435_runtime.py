#!/usr/bin/env python3
"""Host functional test exercising the actual recursive cleaner with mocked mount table.
No PS5 system calls or USB media are modified by this fixture."""
from pathlib import Path
from tempfile import TemporaryDirectory
from subprocess import run
import os
p = Path(__file__).resolve().parent.parent
s = (p/'src/main.c').read_text()
core = s[s.index('static int native_clean_safe_fs('):s.index('/* Operates on a whole native storage root')]
prefix = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#define HOLD_DIR "/system_data/ps4mount"
static int g_stop=0,g_point_all_seen=0;
static int layers_on(const char *p) {(void)p;return 0;}
static int dead_layer(const char *p) {(void)p;return 0;}
static int top_layer(const char *p,const char *stage) {(void)p;(void)stage;return 0;}
static int owned_layer_count(const char *p,const char *stage) {(void)p;(void)stage;return 0;}
static void hold_path(char *b,size_t n,const char *kind,const char *tid){snprintf(b,n,"/mock/%s/%s",kind,tid?tid:"");}
static int nullfs_mount(const char *stage,const char *dir){(void)stage;(void)dir;return 0;}
int unmount(const char *path, int flags){(void)path;(void)flags;return -1;}
int statfs(const char *path,struct statfs *sf){
  memset(sf,0,sizeof(*sf));
  if(strstr(path,"fake_usb_view")) {
    strcpy(sf->f_fstypename,"nullfs");strcpy(sf->f_mntonname,"/mnt/usb0");strcpy(sf->f_mntfromname,"/mnt/usb0/dlc");
  } else {
    strcpy(sf->f_fstypename,"bfs");strcpy(sf->f_mntonname,"/user");strcpy(sf->f_mntfromname,"/dev/ps5_native");
  }
  return 0;
}
int fstatfs(int fd,struct statfs *sf){
  char link[64],buf[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  ssize_t n=readlink(link,buf,sizeof(buf)-1);if(n<0)return -1;buf[n]=0;return statfs(buf,sf);
}
'''
suffix = r'''
int main(int argc,char **argv){
 assert(argc==2);
 int zeroed=0,already=0,failed=0;
 native_clean_walk(argv[1],0,NULL,&zeroed,&already,&failed);
 printf("HOST_RECURSIVE zeroed=%d already=%d failed=%d seen=%d\n",zeroed,already,failed,g_point_all_seen);fflush(stdout);
 assert(zeroed==2 && already==1 && failed==2 && g_point_all_seen==4);
 return 0;
}
'''
with TemporaryDirectory() as d:
    d = Path(d)
    root = d/'internal_addcont'
    (root/'NOT_CUSA'/'weird_label'/'nested_more').mkdir(parents=True)
    (root/'NOT_CUSA'/'weird_label'/'nested_more'/'ac.pkg').write_bytes(b'ABC')
    (root/'abc'/'999').mkdir(parents=True)
    (root/'abc'/'999'/'ac.pkg').write_bytes(b'DEFG')
    (root/'already'/'0').mkdir(parents=True)
    (root/'already'/'0'/'ac.pkg').touch()
    (root/'fake_usb_view'/'one').mkdir(parents=True)
    (root/'fake_usb_view'/'one'/'ac.pkg').write_bytes(b'USB_SAFE')
    (root/'abc'/'999'/'other.pkg').write_bytes(b'DO_NOT_TOUCH')
    (d/'external_usb').mkdir()
    (d/'external_usb'/'ac.pkg').write_bytes(b'ORIGINAL_USB')
    os.symlink(d/'external_usb'/'ac.pkg',root/'abc'/'999'/'ac.pkg_link')
    (root/'abc'/'999'/'symlink_dir').symlink_to(d/'external_usb',target_is_directory=True)
    (root/'notpkg').mkdir()
    (root/'notpkg'/'ac.pkg').symlink_to(d/'external_usb'/'ac.pkg')
    c = d/'test.c';c.write_text(prefix+core+suffix)
    exe = d/'test'
    run(['cc','-std=gnu11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-I'+str(p/'test/host_include'),str(c),'-o',str(exe)],check=True,capture_output=True,text=True)
    result=run([str(exe),str(root)],capture_output=True,text=True)
    assert result.returncode==0, result.stdout+result.stderr
    assert (root/'NOT_CUSA'/'weird_label'/'nested_more'/'ac.pkg').stat().st_size==0
    assert (root/'abc'/'999'/'ac.pkg').stat().st_size==0
    assert (root/'abc'/'999'/'other.pkg').read_bytes()==b'DO_NOT_TOUCH'
    assert (root/'fake_usb_view'/'one'/'ac.pkg').read_bytes()==b'USB_SAFE'
    assert (d/'external_usb'/'ac.pkg').read_bytes()==b'ORIGINAL_USB'
    print(result.stdout.strip().splitlines()[-1])
    print('Host recursive scan: arbitrary labels, nested ac.pkg, non-PKG and USB views protected')
