#!/usr/bin/env python3
"""v0.4.37: host test of independent recursive unlink/create/verify routine.
No mount/unmount calls are permitted. A mounted subtree must be skipped rather
than modifying a USB backing file or reporting success.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
from subprocess import run
import os
p = Path(__file__).resolve().parent.parent
s = (p/'src/main.c').read_text()
core = s[s.index('static int native_clean_safe_fs('):s.index('/* Operates on a whole native storage root')]
assert 'unmount(' not in core and 'nmount(' not in core and 'mount(' not in core
assert 'native_clean_collect' not in core and 'hold_path(' not in core
assert 'unlink(path)' in core and 'O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW' in core
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
static int g_stop=0,g_point_all_seen=0;
static char mock_mount[PATH_MAX];
static int calls_unmount=0,calls_mount=0;
static int layers_on(const char *p) {return !strcmp(p,mock_mount);}
static int dead_layer(const char *p) {(void)p;return 0;}
int unmount(const char *path, int flags){(void)path;(void)flags;calls_unmount++;return -1;}
int nmount(struct iovec *iov, unsigned int niov, int flags){(void)iov;(void)niov;(void)flags;calls_mount++;return -1;}
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
 assert(argc==3);
 snprintf(mock_mount,sizeof(mock_mount),"%s",argv[2]);
 int recreated=0,already=0,failed=0;
 native_clean_walk(argv[1],0,NULL,&recreated,&already,&failed);
 printf("HOST_RECREATE recreated=%d already=%d failed=%d seen=%d mount_calls=%d unmount_calls=%d\n",
        recreated,already,failed,g_point_all_seen,calls_mount,calls_unmount);fflush(stdout);
 assert(recreated==4 && already==0 && failed==2 && g_point_all_seen==4);
 assert(calls_unmount==0 && calls_mount==0);
 return 0;
}
'''
with TemporaryDirectory() as folder:
    d = Path(folder)
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
    (root/'abc'/'999'/'symlink_dir').symlink_to(d/'external_usb',target_is_directory=True)
    (root/'notpkg').mkdir()
    (root/'notpkg'/'ac.pkg').symlink_to(d/'external_usb'/'ac.pkg')
    mount = root/'CUSA00363'
    (mount/'DLC_A').mkdir(parents=True)
    (mount/'DLC_A'/'ac.pkg').write_bytes(b'MOUNTED_DATA_PRESERVED')
    c = d/'test.c';c.write_text(prefix+core+suffix)
    exe = d/'test'
    run(['cc','-std=gnu11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror',
         '-I'+str(p/'test/host_include'),str(c),'-o',str(exe)],check=True,capture_output=True,text=True)
    result=run([str(exe),str(root),str(mount)],capture_output=True,text=True)
    assert result.returncode==0, result.stdout+result.stderr
    assert (root/'NOT_CUSA'/'weird_label'/'nested_more'/'ac.pkg').stat().st_size==0
    assert (root/'abc'/'999'/'ac.pkg').stat().st_size==0
    assert (root/'already'/'0'/'ac.pkg').stat().st_size==0
    assert (root/'notpkg'/'ac.pkg').is_file() and not (root/'notpkg'/'ac.pkg').is_symlink()
    assert (root/'notpkg'/'ac.pkg').stat().st_size==0
    assert (mount/'DLC_A'/'ac.pkg').read_bytes()==b'MOUNTED_DATA_PRESERVED'
    assert (root/'abc'/'999'/'other.pkg').read_bytes()==b'DO_NOT_TOUCH'
    assert (root/'fake_usb_view'/'one'/'ac.pkg').read_bytes()==b'USB_SAFE'
    assert (d/'external_usb'/'ac.pkg').read_bytes()==b'ORIGINAL_USB'
    print(result.stdout.strip().splitlines()[-1])
    print('Host test: files recreated as 0B, overlays skipped and logged, no mounts/unmounts performed.')
