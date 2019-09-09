/*-----------------------------------------------------------------------*\
 |  bios.c  -- CP/M emulator "front-end" for the Z80 emulator  --  runs  |
 |  standard CP/M executables with no changes as long as they use CP/M   |
 |  system calls to do all their work.                                   |
 |                                                                       |
 |  Originally by Kevin Kayes, but he refused any responsibility for it, |
 |  then I later hacked it up, enhanced it, and debugged it.             |
 |                                                                       |
 |  Copyright 1986-1988 by Parag Patel.  All Rights Reserved.            |
 |  Copyright 1994-1995,2000 by CodeGen, Inc.  All Rights Reserved.           |
\*-----------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include "bios.h"
#include "vm.h"
#include "cpmdisc.h"
#include "defs.h"
#include "z80.h"

#ifdef macintosh
#include <stat.h>
#else
#include <sys/stat.h>
#endif

/* definition of: extern unsigned char	cpm_array[]; */
#include "cpm.c"

/* The BDOS/CCP had better be built with these values! */
#define CCP		0xD400 /* (BIOS - 0x1600) */
#define BDOS		0xDC00
#define CBIOS		0xEA00

#define NENTRY		30	/* number of BIOS entries */
#define STACK		0xEF00	/* grows down from here */

/* The first NUMHDISCS drives may be specified as hard-drives. */
#define NUMHDISCS	2
#define NUMDISCS	(MAXDISCS - NUMHDISCS)

#if NUMHDISCS > MAXDISCS
#	error	Too many hard-discs specified here.
#endif

/* disk parameter block information */
#define DPBSIZE		15
#define HDPBLOCK	(CBIOS + NENTRY * 8)
#define DPBLOCK		(HDPBLOCK + DPBSIZE)
#define DIRBUF		(DPBLOCK + DPBSIZE)
#define DPHSIZE		16

/* hard disc parameter info */
#define HDPBASE		(DIRBUF + SECTORSIZE)

#define DPBASE		(HDPBASE + DPHSIZE * NUMHDISCS)
#define CVSIZE		0
#define ALVSIZE		33
#define CVBASE		(DPBASE + DPHSIZE * NUMDISCS)
#define ALVBASE		(CVBASE + CVSIZE * NUMDISCS)

/* ST-506 allocation vector size */
#define HALVBASE	(ALVBASE + ALVSIZE * NUMDISCS)
#define HALVSIZE	306

/* buffer for where to return time/date info */
#define TIMEBUF		(HALVBASE + HALVSIZE * NUMHDISCS)
#define TIMEBUFSIZE	5

/* just a marker for future expansion */
#define END_OF_BIOS	(TIMEBUF + TIMEBUFSIZE)


/* ST-506 HD sector info (floppy defs are in cpmdisc.h for makedisc.c) */
#define	HDSECTORSPERTRACK	64
#define	HDTRACKSPERDISC		610

/* offsets into FCB needed for reading/writing Unix files */
#define FDOFFSET	12
#define BLKOFFSET	16
#define SZOFFSET	20

/* this batch of macros are not used at the moment */
#define CBUFF		CCP+7
#define CIBUFF		CCP+8
#define DEFAULTFCB	0x005C
#define DEFAULTBUF	0x0080
#define IOBYTE		0x0003
#define DISK		0x0004
#define DMA		0x0008
#define USER		0x000A
#define USERSTART	0x0100

struct bios_s {
	vm *vm;
    int silent_exit;

    /* these are for the CP/M BIOS */
    int	drive;
    word dma;
    word track;
    word sector;
    FILE *drives[MAXDISCS];
    long drivelen[MAXDISCS];
};

bios *bios_new(vm *vm)
{
	bios *obj;
	obj = calloc(1, sizeof(bios));
	obj->vm = vm;

	/* initialize the CP/M BIOS data */
	obj->drive = 0;
	obj->dma = 0x80;
	obj->track = 0;
	obj->sector = 1;

    return obj;
}

word bios_get_dma(bios *obj) {
	return obj->dma;
}

void bios_set_dma(bios *obj, word dma) {
	obj->dma = dma;
}

void bios_destroy(bios *obj)
{
	free(obj);
}

void bios_set_silent_exit(bios *obj, int silent_exit)
{
	obj->silent_exit = silent_exit;
}

/* forward declarations: */
static void seldisc(bios *obj, z80info *z80);


static void
closeall(bios *obj)
{
	int	i;

	for (i = 0; i < MAXDISCS; i++)
	{
		if (obj->drives[i] != NULL)
		{
			fclose(obj->drives[i]);
			obj->drives[i] = NULL;
		}
	}
}

void
bios_warmboot(bios *obj, z80info *z80)
{
	unsigned int i;

	closeall(obj);

	if (obj->silent_exit) {
		bios_finish(obj, z80);
	}

	/* load CCP and BDOS into memory (max 0x1600 in size) */
	for (i = 0; i < 0x1600 && i < sizeof cpm_array; i++)
		SETMEM(CCP + i, cpm_array[i]);

	/* try to load CCP/BDOS from disk, but ignore any errors */
	vm_loadfile(z80, "bdos.hex");
	vm_loadfile(z80, "ccp.hex");

	/* CP/M system reset via "JP 00" - entry into BIOS warm-boot */
	SETMEM(0x0000, 0xC3);		/* JP CBIOS+3 */
	SETMEM(0x0001, ((CBIOS + 3) & 0xFF));
	SETMEM(0x0002, ((CBIOS + 3) >> 8));

	/* 0x0003 is the IOBYTE, 0x0004 is the current DISK */
	SETMEM(0x0003, 0x00);
	SETMEM(0x0004, obj->drive);

	/* CP/M syscall via "CALL 05" - entry into BDOS */
	SETMEM(0x0005, 0xC3);		/* JP BDOS+6 */
	SETMEM(0x0006, ((BDOS+6) & 0xFF));
	SETMEM(0x0007, ((BDOS+6) >> 8));

	/* fake BIOS entry points */
	for (i = 0; i < NENTRY; i++)
	{
		/* JP <bios-entry> */
		SETMEM(CBIOS + 3 * i, 0xC3);
		SETMEM(CBIOS + 3 * i + 1, (CBIOS + NENTRY * 3 + i * 5) & 0xFF);
		SETMEM(CBIOS + 3 * i + 2, (CBIOS + NENTRY * 3 + i * 5) >> 8);

		/* LD A,<bios-call> - start of bios-entry */
		SETMEM(CBIOS + NENTRY * 3 + i * 5, 0x3E);
		SETMEM(CBIOS + NENTRY * 3 + i * 5 + 1, i);

		/* OUT A,0FFH - we use port 0xFF to fake the BIOS call */
		SETMEM(CBIOS + NENTRY * 3 + i * 5 + 2, 0xD3);
		SETMEM(CBIOS + NENTRY * 3 + i * 5 + 3, 0xFF);

		/* RET - end of bios-entry */
		SETMEM(CBIOS + NENTRY * 3 + i * 5 + 4, 0xC9);
	}

	/* disc parameter block - a 5Mb ST-506 hard disc */
	SETMEM(HDPBLOCK, HDSECTORSPERTRACK & 0xFF);/* SPT - sectors per track */
	SETMEM(HDPBLOCK + 1, HDSECTORSPERTRACK >> 8);
	SETMEM(HDPBLOCK + 2, 4);	/* BSH - data block shift factor */
	SETMEM(HDPBLOCK + 3, 15);		/* BLM - data block mask */
	SETMEM(HDPBLOCK + 4, 0);		/* EXM - extent mask */
	SETMEM(HDPBLOCK + 5, 2441 & 0xFF);	/* DSM - total drive capacity */
	SETMEM(HDPBLOCK + 6, 2441 >> 8);
	SETMEM(HDPBLOCK + 7, 1023 & 0xFF);	/* DRM - total dir entries */
	SETMEM(HDPBLOCK + 8, 1023 >> 8);
	SETMEM(HDPBLOCK + 9, 0xFF);	/* AL0 - blocks for directory entries */
	SETMEM(HDPBLOCK + 10, 0xFF);	/* AL1 */
	SETMEM(HDPBLOCK + 11, 0x00);	/* CKS - directory check vector */
	SETMEM(HDPBLOCK + 12, 0x00);
	SETMEM(HDPBLOCK + 13, RESERVEDTRACKS & 0xFF);/* OFF - reserved tracks */
	SETMEM(HDPBLOCK + 14, RESERVEDTRACKS >> 8);

	/* disk parameter headers for hard disc */
	for (i = 0; i < NUMHDISCS; i++)
	{
		SETMEM(HDPBASE + DPHSIZE * i, 0x00);	/* XLT */
		SETMEM(HDPBASE + DPHSIZE * i + 1, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 2, 0x00); /* scratch 1 */
		SETMEM(HDPBASE + DPHSIZE * i + 3, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 4, 0x00); /* scratch 2 */
		SETMEM(HDPBASE + DPHSIZE * i + 5, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 6, 0x00); /* scratch 3 */
		SETMEM(HDPBASE + DPHSIZE * i + 7, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 8, DIRBUF & 0xFF); /* DIRBUF */
		SETMEM(HDPBASE + DPHSIZE * i + 9, DIRBUF >> 8);
		SETMEM(HDPBASE + DPHSIZE * i + 10, HDPBLOCK & 0xFF); /* DPB */
		SETMEM(HDPBASE + DPHSIZE * i + 11, HDPBLOCK >> 8);
		SETMEM(HDPBASE + DPHSIZE * i + 12, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 13, 0x00);
		SETMEM(HDPBASE + DPHSIZE * i + 14,
			(HALVBASE + HALVSIZE * i) & 0xFF);
		SETMEM(HDPBASE + DPHSIZE * i + 15,
			(HALVBASE + HALVSIZE * i) >> 8);
	}

	/* disc parameter block - a single-sided single-density 8" 256k disc */
	SETMEM(DPBLOCK, SECTORSPERTRACK & 0xFF); /* SPT - sectors per track */
	SETMEM(DPBLOCK + 1, SECTORSPERTRACK >> 8);
	SETMEM(DPBLOCK + 2, 3);		/* BSH - data block shift factor */
	SETMEM(DPBLOCK + 3, 7);			/* BLM - data block mask */
	SETMEM(DPBLOCK + 4, 0);			/* EXM - extent mask */
	SETMEM(DPBLOCK + 5, 242);	/* DSM - total capacity of drive */
	SETMEM(DPBLOCK + 6, 0);
	SETMEM(DPBLOCK + 7, 63);	/* DRM - total directory entries */
	SETMEM(DPBLOCK + 8, 0);
	SETMEM(DPBLOCK + 9, 0xC0);	/* AL0 - blocks for directory entries */
	SETMEM(DPBLOCK + 10, 0x00);	/* AL1 */
	SETMEM(DPBLOCK + 11, 0x00);	/* CKS - directory check vector */
	SETMEM(DPBLOCK + 12, 0x00);
	SETMEM(DPBLOCK + 13, RESERVEDTRACKS & 0xFF); /* OFF - reserved tracks */
	SETMEM(DPBLOCK + 14, RESERVEDTRACKS >> 8);

	/* disc parameter headers */
	for (i = 0; i < NUMDISCS; i++)
	{
		SETMEM(DPBASE + DPHSIZE * i, 0x00);	/* XLT */
		SETMEM(DPBASE + DPHSIZE * i + 1, 0x00);
		SETMEM(DPBASE + DPHSIZE * i + 2, 0x00); /* scratch 1 */
		SETMEM(DPBASE + DPHSIZE * i + 3, 0x00);
		SETMEM(DPBASE + DPHSIZE * i + 4, 0x00); /* scratch 2 */
		SETMEM(DPBASE + DPHSIZE * i + 5, 0x00);
		SETMEM(DPBASE + DPHSIZE * i + 6, 0x00); /* scratch 3 */
		SETMEM(DPBASE + DPHSIZE * i + 7, 0x00);
		SETMEM(DPBASE + DPHSIZE * i + 8, DIRBUF & 0xFF); /* DIRBUF */
		SETMEM(DPBASE + DPHSIZE * i + 9, DIRBUF >> 8);
		SETMEM(DPBASE + DPHSIZE * i + 10, DPBLOCK & 0xFF); /* DPB */
		SETMEM(DPBASE + DPHSIZE * i + 11, DPBLOCK >> 8);
#if (CVSIZE == 0)
		SETMEM(DPBASE + DPHSIZE * i + 12, 0x00);
		SETMEM(DPBASE + DPHSIZE * i + 13, 0x00);
#else
		SETMEM(DPBASE + DPHSIZE * i + 12,
			(CVBASE + CVSIZE * i) & 0xFF);
		SETMEM(DPBASE + DPHSIZE * i + 13,
			(CVBASE + CVSIZE * i) >> 8);
#endif
		SETMEM(DPBASE + DPHSIZE * i + 14,
			(ALVBASE + ALVSIZE * i) & 0xFF);
		SETMEM(DPBASE + DPHSIZE * i + 15,
			(ALVBASE + ALVSIZE * i) >> 8);
	}

	/* set up the stack for an 8-level RET to do a system reset */
	SP = STACK;

	for (i = 0; i < 8; i++)
	{
		/* push reboot entry (CBIOS + 3) onto stack */
		--SP;
		SETMEM(SP, (CBIOS + 3) >> 8);
		--SP;
		SETMEM(SP, (CBIOS + 3) & 0xFF);
	}

	/* set up the default disk (A:) and dma address */
	obj->dma = 0x0080;

	/* and all our default drive info */
	obj->track = 0;
	obj->sector = 1;

	/* make sure the current file/disk is open */
	B = 0;
	C = obj->drive;
	seldisc(obj, z80);

	PC = CCP;
}

static void
boot(bios *obj, z80info *z80)
{
	obj->drive = 0;
	bios_warmboot(obj, z80);
}

void
bios_sysreset(bios *obj, z80info *z80)
{
	boot(obj, z80);
}

static void
consstat(bios *obj, z80info *z80)
{
    (void)obj;
	vm_input(obj->vm, z80, 0x01, 0x01, &A);
}

static void
consin(bios *obj, z80info *z80)
{
    (void)obj;
	vm_input(obj->vm, z80, 0x00, 0x00, &A);

/* What is this for? It messing up Ctrl-S...
	if (A == CNTL('S'))
		input(z80, 0x00, 0x00, &A);
*/
}

static void
consout(bios *obj, z80info *z80)
{
    (void)obj;
	vm_output(obj->vm, z80, 0x00, 0x00, C & 0x7F);
}

/* list character in C */
static void
list(bios *obj, z80info *z80)
{
	static FILE *fp = NULL;
    (void)obj;

	if (fp == NULL)
	{
		fp = fopen("list", "w");

		if (fp == NULL)
			return;
	}

	/* close up on EOF */
	if (C == CNTL('D') || C == '\0')
	{
		fclose(fp);
		fp = NULL;
		return;
	}

	putc(C, fp);
}

/* punch character in C */
static void
punch(bios *obj, z80info *z80)
{
    (void)obj;
	(void)z80;
}

/* return reader char in A, ^Z is EOF */
static void
reader(bios *obj, z80info *z80)
{
    (void)obj;
	(void)z80;
	A = CNTL('Z');
}

static void
home(bios *obj, z80info *z80)
{
	(void)z80;
	obj->track = 0;
	obj->sector = 1;
}

/* Open disk image */

static void
realizedisk(bios *obj)
{
	int drive = obj->drive;
	char drivestr[80];

	strcpy(drivestr, drive < NUMHDISCS ? "A-Hdrive" : "A-drive");
	drivestr[0] += drive; /* set the 1st letter to the drive name */

	if (obj->drives[drive] == NULL)
	{
		struct stat statbuf;
		long secs;
		FILE *fp;

		fp = fopen(drivestr, "rb+");

		if (fp == NULL)
			fp = fopen(drivestr, "wb+");

		if (fp == NULL)
		{
			fprintf(stderr, "seldisc(): Cannot open file '%s'!\r\n",
					drivestr);
			return;
		}

		if (stat(drivestr, &statbuf) < 0)
		{
			fprintf(stderr, "seldisc(): Cannot stat file '%s'!\r\n",
					drivestr);
			fclose(fp);
			return;
		}

		secs = statbuf.st_size / SECTORSIZE;

		if (secs == 0)
		{
			char buf[SECTORSIZE];
			memset(buf, 0xE5, SECTORSIZE);

			if (fwrite(buf, 1, SECTORSIZE, fp) != SECTORSIZE)
			{
				fprintf(stderr, "seldisc(): Cannot create file '%s'!\r\n",
						drivestr);

				fclose(fp);
				return;
			}

			secs = 1;
		}

		/* printf(stderr,"\r\nOpen %s on drive %d\n", drivestr, drive); */

		obj->drives[drive] = fp;
		obj->drivelen[drive] = secs * SECTORSIZE;
	}
}

static void
seldisc(bios *obj, z80info *z80)
{
	H = 0;
	L = 0;

	if (C >= MAXDISCS)
	{
		fprintf(stderr, "seldisc(): Attempt to open bogus drive %d\r\n",
			C);
		return;
	}

	obj->drive = C;

	if (obj->drive < NUMHDISCS)
	{
	    L = (HDPBASE + DPHSIZE * C) & 0xFF;
	    H = (HDPBASE + DPHSIZE * C) >> 8;
	}
	else
	{
	    L = (DPBASE + DPHSIZE * C) & 0xFF;
	    H = (DPBASE + DPHSIZE * C) >> 8;
	}

	home(obj, z80);
}

static void
settrack(bios *obj, z80info *z80)
{
	int tracks = (obj->drive < NUMHDISCS) ?
			HDTRACKSPERDISC : TRACKSPERDISC;

    (void)obj;
	obj->track = (B << 8) + C;

	if (obj->track < RESERVEDTRACKS || obj->track >= tracks)
		fprintf(stderr, "settrack(): bogus track %d!\r\n",
				obj->track);
}

static void
setsector(bios *obj, z80info *z80)
{
	int sectors = (obj->drive < NUMHDISCS) ?
			HDSECTORSPERTRACK : SECTORSPERTRACK;
    (void)obj;

    obj->sector = (B << 8) + C;

	if (obj->sector < SECTOROFFSET || obj->sector > sectors)
		fprintf(stderr, "setsector(): bogus sector %d!\r\n",
				obj->sector);
}

static void
setdma(bios *obj, z80info *z80)
{
    (void)obj;
    obj->dma = (B << 8) + C;
}


static void
rdsector(bios *obj, z80info *z80)
{
	int n;
	int drive = obj->drive;
	int sectors = (drive < NUMHDISCS) ? HDSECTORSPERTRACK : SECTORSPERTRACK;
	long offset = SECTORSIZE * ((long)obj->sector - SECTOROFFSET +
			sectors * ((long)obj->track - TRACKOFFSET));
	FILE *fp;
	long len;
    (void)obj;
	realizedisk(obj);
	fp = obj->drives[drive];
	len = obj->drivelen[drive];

	if (fp == NULL)
	{
		fprintf(stderr, "rdsector(): file/drive %d not open!\r\n",
			drive);
		A = 1;
		return;
	}

	if (len && offset >= len)
	{
	    memset(&(z80->mem[obj->dma]), 0xE5, SECTORSIZE);
	    A = 0;
	    return;
	}

	if (fseek(fp, offset, SEEK_SET) != 0)
	{
		fprintf(stderr, "rdsector(): fseek failure offset=0x%lX!\r\n",
			offset);
		A = 1;
		return;
	}

	n = fread(&(z80->mem[obj->dma]), 1, SECTORSIZE, fp);

	if (n != SECTORSIZE)
	{
		fprintf(stderr, "rdsector(): read failure %d!\r\n", n);
		A = 1;
	}
	else
		A = 0;
}


static void
wrsector(bios *obj, z80info *z80)
{
	int drive = obj->drive;
	int sectors = (drive < NUMHDISCS) ? HDSECTORSPERTRACK : SECTORSPERTRACK;
	long offset = SECTORSIZE * ((long)obj->sector - SECTOROFFSET +
			sectors * ((long)obj->track - TRACKOFFSET));
	FILE *fp;
	long len;
	realizedisk(obj);
	fp = obj->drives[drive];
	len = obj->drivelen[drive];

	if (fp == NULL)
	{
		fprintf(stderr, "wrsector(): file/drive %d not open!\r\n",
			drive);
		A = 1;
		return;
	}

	if (len && offset > len)
	{
		char buf[SECTORSIZE];

		if (fseek(fp, len, SEEK_SET) != 0)
		{
			fprintf(stderr, "wrsector(): fseek failure offset=0x%lX!\r\n",
				len);
			A = 1;
			return;
		}

		memset(buf, 0xE5, SECTORSIZE);

		while (offset > len)
		{
			if (fwrite(buf, 1, SECTORSIZE, fp) != SECTORSIZE)
			{
				fprintf(stderr, "wrsector(): write failure!\r\n");
				A = 1;
				return;
			}

			len += SECTORSIZE;
			obj->drivelen[drive] = len;
		}
	}

	if (fseek(fp, offset, SEEK_SET) != 0)
	{
		fprintf(stderr, "wrsector(): fseek failure offset=0x%lX!\r\n",
			offset);
		A = 1;
		return;
	}

	if (fwrite(&(z80->mem[obj->dma]), 1, SECTORSIZE, fp) != SECTORSIZE)
	{
		fprintf(stderr, "wrsector(): write failure!\r\n");
		A = 1;
	}
	else
	{
		A = 0;

		if (offset + SECTORSIZE > len)
			obj->drivelen[drive] = offset + SECTORSIZE;
	}
}

static void
secttran(bios *obj, z80info *z80)
{
	if (obj->drive < NUMHDISCS)
	{
		/* simple sector translation for hard disc */
		HL = BC + 1;

		if (BC >= HDSECTORSPERTRACK)
			fprintf(stderr, "secttran(): bogus sector %d!\r\n", BC);
	}
	else
	{
		/* we do not need to use DE to find our translation table */
		HL = sectorxlat[BC];

		if (BC >= SECTORSPERTRACK)
			fprintf(stderr, "secttran(): bogus sector %d!\r\n", BC);
	}
}

static void
liststat(bios *obj, z80info *z80)
{
    (void)obj;
	A = 0xFF;
}

/* These two routines read and write ints at arbitrary aligned addrs.
 * The values are stored in the z80 in little-endian order regardless
 * of the byte-order on the host.
 */
static int
addr2int(unsigned char *addr)
{
	unsigned char *a = (unsigned char*)addr;
	unsigned int t;

	t = a[0] | (a[1] << 8) | (a[2] << 16) | (a[3] << 24);
	return (int)t;
}

static void
int2addr(unsigned char *addr, int val)
{
	unsigned char *a = (unsigned char*)addr;
	unsigned int t = (unsigned int)val;

	a[0] = t & 0xFF;
	a[1] = (t >> 8) & 0xFF;
	a[2] = (t >> 16) & 0xFF;
	a[3] = (t >> 24) & 0xFF;
}

/* Allocate file pointers - index is stored in DE */

#define CPM_FILES 4

FILE *cpm_file[CPM_FILES];

static int cpm_file_alloc(FILE *f)
{
	int x;
	for (x = 0; x != CPM_FILES; ++x)
		if (!cpm_file[x]) {
			cpm_file[x] = f;
			return x;
		}
	return -1;
}

static FILE *cpm_file_get(int idx)
{
	if (idx < 0 || idx > CPM_FILES)
		return 0;
	else
		return cpm_file[idx];
}

static int cpm_file_free(int x)
{
	if (x >= 0 && x < CPM_FILES && cpm_file[x]) {
		int rtn = fclose(cpm_file[x]);
		cpm_file[x] = 0;
		return rtn;
	} else {
		return -1;
	}
}

/* DE points to a CP/M FCB.
   On return, A contains 0 if all went well, 0xFF otherwise.
   The algorithm uses the FCB to store info about the UNIX file.
 */
static void
openunix(bios *obj, z80info *z80)
{
	char filename[20], *fp;
	byte *cp;
	int i;
	FILE *fd;
	int fd_no;

    (void)obj;
	cp = &(z80->mem[DE + 1]);
	fp = filename;

	for (i = 0; (*cp != ' ') && (i < 8); i++)
		*fp++ = tolower(*cp++);

	cp = &(z80->mem[DE + 9]);

	if (*cp != ' ')
	{
		*fp++ = '.';

		for (i = 0; (*cp != ' ') && (i < 3); i++)
			*fp++ = tolower(*cp++);
	}

	*fp = 0;
	A = 0xFF;

	/* if file is not readable, try opening it read-only */
	if ((fd = fopen(filename, "rb+")) == NULL)
		if ((fd = fopen(filename, "rb")) == NULL)
			return;

	fd_no = cpm_file_alloc(fd);
	if (fd_no != -1)
		A = 0;

	int2addr(&z80->mem[DE + FDOFFSET], fd_no);
	int2addr(&z80->mem[DE + BLKOFFSET], 0);
	int2addr(&z80->mem[DE + SZOFFSET], 0);
}


/* DE points to a CP/M FCB.
   On return, A contains 0 if all went well, 0xFF otherwise.
   The algorithm uses the FCB to store info about the UNIX file.
 */
static void
createunix(bios *obj, z80info *z80)
{
	char filename[20], *fp;
	byte *cp;
	int i;
	FILE *fd;
	int fd_no;

    (void)obj;
	cp = &(z80->mem[DE + 1]);
	fp = filename;

	for (i = 0; (*cp != ' ') && (i < 8); i++)
		*fp++ = tolower(*cp++);

	cp = &(z80->mem[DE + 9]);

	if (*cp != ' ')
	{
		*fp++ = '.';

		for (i = 0; (*cp != ' ') && (i < 3); i++)
			*fp++ = tolower(*cp++);
	}

	*fp = 0;
	A = 0xFF;

	if ((fd = fopen(filename, "wb+")) == NULL)
		return;

	fd_no = cpm_file_alloc(fd);
	if (fd_no != -1)
		A = 0;

	int2addr(&z80->mem[DE + FDOFFSET], fd_no);
	int2addr(&z80->mem[DE + BLKOFFSET], 0);
	int2addr(&z80->mem[DE + SZOFFSET], 0);
}


/* DE points to a CP/M FCB.
   On return, A contains 0 if all went well, 0xFF otherwise.
   The algorithm uses the FCB to store info about the UNIX file.
 */
static void
rdunix(bios *obj, z80info *z80)
{
	byte *cp;
	int i, blk, size;
	FILE *fd;
	int fd_no;
  
    (void)obj;
	cp = &(z80->mem[obj->dma]);
	fd = cpm_file_get((fd_no = addr2int(&z80->mem[DE + FDOFFSET])));
	blk = addr2int(&z80->mem[DE + BLKOFFSET]);
	size = addr2int(&z80->mem[DE + SZOFFSET]);

	A = 0xFF;

	if (!fd)
		return;

	if (fseek(fd, (long)blk << 7, SEEK_SET) != 0)
		return;

	i = fread(cp, 1, SECTORSIZE, fd);
	size = i;

	if (i == 0)
		return;

	for (; i < SECTORSIZE; i++)
		cp[i] = CNTL('Z');

	A = 0;
	blk += 1;

	int2addr(&z80->mem[DE + FDOFFSET], fd_no);
	int2addr(&z80->mem[DE + BLKOFFSET], blk);
	int2addr(&z80->mem[DE + SZOFFSET], size);
}


/* DE points to a CP/M FCB.
   On return, A contains 0 if all went well, 0xFF otherwise.
   The algorithm uses the FCB to store info about the UNIX file.
 */
static void
wrunix(bios *obj, z80info *z80)
{
	byte *cp;
	int i, blk, size;
	FILE *fd;
	int fd_no;

    (void)obj;
	cp = &(z80->mem[obj->dma]);
	fd = cpm_file_get((fd_no = addr2int(&z80->mem[DE + FDOFFSET])));
	blk = addr2int(&z80->mem[DE + BLKOFFSET]);
	size = addr2int(&z80->mem[DE + SZOFFSET]);

	A = 0xFF;

	if (fseek(fd, (long)blk << 7, SEEK_SET) != 0)
		return;

	i = fwrite(cp, 1, size = SECTORSIZE, fd);

	if (i != SECTORSIZE)
		return;

	A = 0;
	blk += 1;

	int2addr(&z80->mem[DE + FDOFFSET], fd_no);
	int2addr(&z80->mem[DE + BLKOFFSET], blk);
	int2addr(&z80->mem[DE + SZOFFSET], size);
}


/* DE points to a CP/M FCB.
   On return, A contains 0 if all went well, 0xFF otherwise.
 */
static void
closeunix(bios *obj, z80info *z80)
{
	int fd_no;

    (void)obj;
	fd_no = addr2int(&z80->mem[DE + FDOFFSET]);
	A = 0xFF;

	if (cpm_file_free(fd_no))
		return;

	A = 0;
}

/* clean up and quit - never returns */
void
bios_finish(bios *obj, z80info *z80)
{
	(void)z80;
	vm_resetterm(obj->vm);

	/* TODO: Provide a more graceful shutdown across all the layers */
	z80_destroy(z80);
	vm_destroy(obj->vm);

	exit(0);
}

/*  Get/set the time - although only the get-time part is implemented.
    If C==0, then get the time, else of C==0xFF, then set the time.
    HL returns a pointer to our time table:
	HL+0:DATE LSB SINCE 1,1,1978
	HL+1:DATE MSB
	HL+2:HOURS  (BCD)
	HL+3:MINUTES (BCD)
	HL+4:SECONDS (BCD)
 */
static void
dotime(bios *obj, z80info *z80)
{
    time_t now;
    struct tm *t;
    word days;
    int y;
    (void)obj;

    if (C != 0)		/* do not support setting the time yet */
		return;

    time(&now);
    t = localtime(&now);

    /* days since Jan 1, 1978 + one since tm_yday starts at zero */
    days = (t->tm_year - 78) * 365 + t->tm_yday + 1;

    /* add in the number of days for the leap years - dumb but accurate */
    for (y = 78; y < t->tm_year; y++)
		if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
			days++;

    HL = TIMEBUF;
    SETMEM(HL + 0, days & 0xFF);
    SETMEM(HL + 1, days >> 8);
    SETMEM(HL + 2, ((t->tm_hour / 10) << 4) + (t->tm_hour % 10));
    SETMEM(HL + 3, ((t->tm_min / 10) << 4) + (t->tm_min % 10));
    SETMEM(HL + 4, ((t->tm_sec / 10) << 4) + (t->tm_sec % 10));
}

void
bios_call(bios *obj, z80info *z80, unsigned int fn)
{
	static void (*bioscall[])(bios *, z80info *) =
	{
		boot,		/* 0 */
		bios_warmboot,	/* 1 */
		consstat,	/* 2 */
		consin,		/* 3 */
		consout,	/* 4 */
		list,		/* 5 */
		punch,		/* 6 */
		reader,		/* 7 */
		home,		/* 8 */
		seldisc,	/* 9 */
		settrack,	/* 10 */
		setsector,	/* 11 */
		setdma,		/* 12 */
		rdsector,	/* 13 */
		wrsector,	/* 14 */
		liststat,	/* 15 */
		secttran,	/* 16 */
		openunix,	/* 17 */
		createunix,	/* 18 */
		rdunix,		/* 19 */
		wrunix,		/* 20 */
		closeunix,	/* 21 */
		bios_finish,		/* 22 */
		dotime		/* 23 */
	};

	if (fn >= sizeof bioscall / sizeof *bioscall)
	{
		fprintf(stderr, "Illegal BIOS call %d\r\n", fn);
		return;
	}

	bioscall[fn](obj, z80);
	/* let z80 handle return */
}
