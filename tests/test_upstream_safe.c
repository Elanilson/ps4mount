/* Host tests for a strict, read-only subset of upstream 0.4.0 config parsing. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/upstream_safe.h"
int main(void) {
    char path[128], body[1200];
    assert(!ps4mount_safe_path("/",path,sizeof(path))&&!strcmp(path,""));
    assert(!ps4mount_safe_path("//Meus Jogos/2026/",path,sizeof(path))&&!strcmp(path,"Meus Jogos/2026"));
    assert(!ps4mount_safe_path("games/PS4 backups",path,sizeof(path)));
    assert(ps4mount_safe_path("../../system_data",path,sizeof(path)));
    assert(ps4mount_safe_path("a/../b",path,sizeof(path)));
    assert(ps4mount_safe_path("a/./b",path,sizeof(path)));
    assert(ps4mount_safe_path("a//b",path,sizeof(path)));
    assert(ps4mount_safe_path("C:\\Windows",path,sizeof(path)));
    assert(ps4mount_safe_path("ps4/CUSA00000",path,sizeof(path)));
    assert(ps4mount_safe_path("PS4",path,sizeof(path)));
    assert(ps4mount_safe_path("a/%00",path,4));
    assert(!ps4mount_url_decode("%2FMeus+Jogos%0A%2F",strlen("%2FMeus+Jogos%0A%2F"),body,sizeof(body)));
    assert(!strcmp(body,"/Meus Jogos\n/"));
    assert(ps4mount_url_decode("%2x",3,body,sizeof(body)));
    assert(ps4mount_url_decode("%00",3,body,sizeof(body)));
    assert(ps4mount_url_decode("%2F",3,body,1));
    assert(ps4mount_url_decode("%",1,body,sizeof(body)));
    puts("PASS: upstream settings paths, URL decoding, traversal/poisoning checks");
    return 0;
}
