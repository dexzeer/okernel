#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
int main(int argc, char **argv) {
    struct utsname u; uname(&u);
    printf("glibc hello: argc=%d argv0=%s sys=%s machine=%s\n", argc, argv[0], u.sysname, u.machine);
    char *p = malloc(100000); memset(p, 7, 100000); printf("malloc ok %d\n", p[99999]);
    printf("pid>0: %d\n", getpid() > 0);
    return 3;
}
