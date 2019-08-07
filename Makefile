# optional CFLAGS include: -O -g -Wall
# -DNO_LARGE_SWITCH	compiler cannot handle really big switch statements
#			so break them into smaller pieces
# -DENDIAN_LITTLE	machine's byte-sex is like x86 instead of 68k
# -DPOSIX_TTY		use Posix termios instead of older termio (FreeBSD)
# -DMEM_BREAK		support memory-mapped I/O and breakpoints,
#				which will noticably slow down emulation

ifeq ($(OS),Windows_NT)
  EXE 		:= .exe
else
  EXE 		:=
endif

CC = gcc
CFLAGS = -g -pipe -Wall -Wextra -pedantic -ansi \
	 -D_POSIX_C_SOURCE=200809L -DPOSIX_TTY \
	 -DENDIAN_LITTLE -DMEM_BREAK
LDFLAGS = 

FILES = README.md Makefile A-Hdrive B-Hdrive cpmws.png \
	bdos.c bios.c cpm.c cpmdisc.h defs.h disassem.c main.c vt.c vt.h z80.c \
	bye.mac getunix.mac putunix.mac cpmtool.c

OBJS =	bios.o \
	disassem.o \
	main.o \
	vt.o \
	bdos.o \
	z80.o

all: cpm$(EXE) cpmtool$(EXE)

cpmtool$(EXE): cpmtool.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o cpmtool$(EXE) cpmtool.o

cpm$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o cpm$(EXE) $(OBJS)


bios.o:		bios.c defs.h cpmdisc.h cpm.c
z80.o:		z80.c defs.h
disassem.o:	disassem.c defs.h
main.o:		main.c defs.h

clean:
	rm -f cpm$(EXE) cpmtool$(EXE) *.o *~

tags:	$(FILES)
	cxxtags *.[hc]

tar:
	tar -zcf cpm.tgz $(FILES)

files:
	@echo $(FILES)

difflist:
	@for f in $(FILES); do rcsdiff -q $$f >/dev/null || echo $$f; done
