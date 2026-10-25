// Entry point for test programs that use our msvcrt.dll: C runtime start-up, then main().
#ifndef CRTMAIN_H
#define CRTMAIN_H
extern int __getmainargs(int *argc, char ***argv, char ***env, int wild, void *si);
extern void exit(int code);
int main(int argc, char **argv);
int entry(void *peb) {
    (void)peb;
    int argc; char **argv, **env;
    __getmainargs(&argc, &argv, &env, 0, 0);
    exit(main(argc, argv));
    return 0;
}
#endif
