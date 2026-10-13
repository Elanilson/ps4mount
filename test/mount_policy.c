#include <assert.h>
#include <stdio.h>
#include "mount_policy.h"

int main(void) {
    const char *hold = "/system_data/ps4mount/app/CUSA12345";
    assert(mount_layer_is_owned("nullfs", hold, hold));
    assert(!mount_layer_is_owned("nullfs", "/mnt/usb0/ps4/CUSA12345", hold));
    assert(!mount_layer_is_owned("nullfs", "/system_data/ps4mount/app/CUSA99999", hold));
    assert(!mount_layer_is_owned("ufs", hold, hold));
    assert(!mount_layer_is_owned(NULL, hold, hold));
    assert(!mount_layer_is_owned("nullfs", NULL, hold));
    puts("mount ownership: 6 checks, 0 failed");
    return 0;
}
