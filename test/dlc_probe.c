#include "../src/dlc_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    dlc_probe_target_t t;
    const char *g="CUSA34384",*d="CUSA34384",*l="GOWRAGNAROKDDE00";
    const char *roots[]={"","/user/app/","/user/addcont/","/user/addcont/",
        "/user/addcont/CUSA34384/","/user/addcont/CUSA34384/GOWRAGNAROKDDE00/"};
    for(int i=1;i<=5;i++) {
        assert(!dlc_probe_target(i,g,d,l,&t));
        assert(!strcmp(t.root,roots[i]));
        assert(!strcmp(t.title,g));
    }
    assert(dlc_probe_target(6,g,d,l,&t)<0);
    assert(dlc_probe_target(1,"../../bad",d,l,&t)<0);
    assert(dlc_probe_target(1,g,d,"../../bad",&t)<0);
    puts("Targeted DLC experimental method matrix: OK");
}
