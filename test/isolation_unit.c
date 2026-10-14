#include "isolation.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmp[] = "/tmp/ps4mount-isolation-XXXXXX";
static char p[4096],hold[4096],state[4096];
static void make_dir(const char *dir) { assert(mkdir(dir,0700)==0); }
static void link_at(const char *dir, const char *name) {
  snprintf(p,sizeof(p),"%s/%s",dir,name);
  assert(symlink("/mnt/usb0/ps4/test.pkg",p)==0);
}
static void expected(const char *dir,const char *name,int exists) {
  struct stat st; snprintf(p,sizeof(p),"%s/%s",dir,name);
  assert((lstat(p,&st)==0) == exists);
  if(exists) assert(S_ISLNK(st.st_mode));
}
int main(void){
 assert(mkdtemp(tmp));
 char path[4096], dir[4096],group[4096];
 snprintf(hold,sizeof(hold),"%s/hold",tmp);make_dir(hold);
 snprintf(state,sizeof(state),"%s/state",tmp);make_dir(state);
 for(int k=0;k<3;k++) {
   const char *kind=(const char*[]){"app","patch","addcont"}[k];
   snprintf(path,sizeof(path),"%s/%s",hold,kind);make_dir(path);
   for(int j=0;j<2;j++) {
     const char *title=j?"CUSA03842":"CUSA34384";
     snprintf(group,sizeof(group),"%s/%s",path,title);make_dir(group);
     if(k==2) {snprintf(dir,sizeof(dir),"%s/DLC00001",group);make_dir(dir);} else strcpy(dir,group);
     link_at(dir,(const char*[]){"app.pkg","patch.pkg","ac.pkg"}[k]);
   }
 }
 isolation_report_t r;
 assert(isolation_enable(hold,state,"CUSA34384",&r)==0);
 assert(r.changed==3);
 assert(isolation_status(hold,state,&r)==1);
 assert(r.already_hidden==3);
 for(int k=0;k<3;k++) {
   const char *kind=(const char*[]){"app","patch","addcont"}[k];
   const char *name=(const char*[]){"app.pkg","patch.pkg","ac.pkg"}[k];
   snprintf(dir,sizeof(dir),"%s/%s/CUSA03842",hold,kind);
   if(k==2) strcat(dir,"/DLC00001");
   expected(dir,name,0);
   char hidden[100];snprintf(hidden,sizeof(hidden),"__ps4mount_hidden__%s",name);
   expected(dir,hidden,1);
 }
 assert(isolation_enable(hold,state,"CUSA34384",&r)==-1);
 assert(isolation_enable(hold,state,"CUSA03842",&r)==-1);
 // Simulate a reboot: marker and hidden links survive, restoration works.
 assert(isolation_restore(hold,state,&r)==0);
 assert(r.changed==3);
 assert(isolation_status(hold,state,&r)==0);
 assert(r.already_hidden==0);
 // Preflight conflict: a hidden name cannot be overwritten.
 snprintf(dir,sizeof(dir),"%s/app/CUSA03842",hold);
 link_at(dir,"__ps4mount_hidden__app.pkg");
 assert(isolation_enable(hold,state,"CUSA34384",&r)==-1);
 expected(dir,"app.pkg",1);
 assert(isolation_restore(hold,state,&r)==-1); // would clobber, refuses to restore
 assert(unlink(strcat(strcpy(p,dir),"/app.pkg"))==0);
 assert(isolation_restore(hold,state,&r)==0); // recover the leftover link even without marker
 expected(dir,"app.pkg",1);
 printf("[isolation-test] PASS: hide, restore, restart, conflict protection, orphan recovery\n");
 return 0;
}
