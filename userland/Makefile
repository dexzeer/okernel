# userland/ Makefile — builds freestanding i386 ET_EXEC programs for /bin.
# Same discipline as the kernel: no libc, no PIC, no red zone, no stack
# protector. Host gcc+multilib emits valid ET_EXEC i386 (verified readelf).
# `make` builds all programs; `make gen` regenerates bindata C includes.

CC = gcc
CFLAGS = -m32 -nostdlib -static -Os -fno-builtin -fno-stack-protector \
         -mno-red-zone -fno-pic -fno-pie -Wall
LDFLAGS = -e _start

PROGS = hello forktest pipetest exectest sh init forkexec dbg2 dbg3

all: $(PROGS)

hello: hello.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

forktest: forktest.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

pipetest: pipetest.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

exectest: exectest.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

sh: sh.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

init: init.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

forkexec: forkexec.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

dbg2: dbg2.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

dbg3: dbg3.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# Emit bindata C include for one program: xxd -i <prog> > gen_<prog>.h
gen:
	for p in $(PROGS); do xxd -i $$p > gen_$$p.h; done

clean:
	rm -f $(PROGS) gen_*.h *.s

.PHONY: all gen clean
