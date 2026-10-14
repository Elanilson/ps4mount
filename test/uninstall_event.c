#include "uninstall_event.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    unsigned rc=0;
    assert(ps4mount_uninstall_result("EP0102-CUSA03842_00-BH70000000DLC001, 0x80020042)", &rc));
    assert(rc == 0x80020042u);
    assert(ps4mount_uninstall_result("EP0102-CUSA03842_00-BH70000000DLC001, 0x00000000)", &rc));
    assert(rc == 0);
    assert(!ps4mount_uninstall_result("EP0102-CUSA03842_00-BH70000000DLC001)", &rc));
    assert(!ps4mount_uninstall_result("EP0102-CUSA03842_00-BH70000000DLC001, bad)", &rc));
    assert(!ps4mount_uninstall_result("EP0102-CUSA03842_00-BH70000000DLC001, 0x00000000", &rc));
    puts("DLC uninstall event return-code parser: OK");
}
