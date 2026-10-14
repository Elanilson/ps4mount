#include "ps5_appinst_pkg.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
int main(void) {
    assert(offsetof(ps5_install_status_t, error_info) == 60);
    assert(offsetof(ps5_install_status_t, local_copy_percent) == 592);
    assert(offsetof(ps5_install_status_t, is_copy_only) == 596);
    printf("PS5 AppInstUtil ABI: struct sizes and field offsets checked\n");
    return 0;
}
