# userland/ Makefile — builds freestanding i386 ET_EXEC programs for /bin.
# Same discipline as the kernel: no libc, no PIC, no red zone, no stack
# protector. Host gcc+multilib emits valid ET_EXEC i386 (verified readelf).
# `make` builds all programs; `make gen` regenerates bindata C includes.

CC = gcc
CFLAGS = -m32 -nostdlib -static -Os -fno-builtin -fno-stack-protector \
         -mno-red-zone -fno-pic -fno-pie -Wall
LDFLAGS = -e _start

PROGS = hello forktest pipetest exectest sh init forkexec dbg2 dbg3

# Ring-3 ABI library: every userland program links usys.o, so the INT 0x80
# trap site lives in exactly one translation unit (usys.c).
USYS_OBJ = usys.o

all: $(PROGS)

usys.o: usys.c usys.h
	$(CC) $(CFLAGS) -c usys.c -o usys.o

$(PROGS): %: %.c $(USYS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(USYS_OBJ)

# Emit bindata C include for one program: xxd -i <prog> > gen_<prog>.h
gen:
	for p in $(PROGS); do xxd -i $$p > gen_$$p.h; done

clean:
	rm -f $(PROGS) usys.o gen_*.h *.s

.PHONY: all gen clean
