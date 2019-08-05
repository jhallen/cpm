/* CP/M disk */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 128

/* Layout is:
 *    Zero or more reserved tracks (off)
 *    One or more data blocks (dsm+1), power of 2 at least 1K
 *       Directory starts with block 0
 *       Directory has (drm+1) entries
 *    Spare sectors (ignored by CP/M)
 *
 * CP/M 1.4 on 8 inch 250.25K disk:
 *    77 tracks
 *    26 128-byte sectors per track, software skewed
 *      skew table [1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22]
 *    2 reserved tracks
 *    2 1K directory blocks, giving 64 directory entries
 *    243 1K data blocks numbered 2 - 242
 *    6 extra sectors
 */

struct cpm_dpb {
	unsigned short spt; /* 128 byte sectors per track (26) */
	unsigned char bsh; /* Block shift: 3 = 1K, 4 = 2K, etc. (3) */
	unsigned char blm; /* Block mask: 0x7 = 1K, 0xF = 2K, etc. (7) */
	unsigned char exm; /* Extent mask: full record count = 128 * (EX & exm) + rc */
	unsigned short dsm; /* Number of blocks on the disk - 1 */
	unsigned short drm; /* Number of directory entries - 1 */
	unsigned char al0; /* Directory allocation bitmap, first byte */
	unsigned char al1; /* Directory allocation bitmap, second byte */
	unsigned char cks; /* Checksum vector size: 0 for fixed disk */
	unsigned short off; /* Offset, number of reserved tracks */
	unsigned char skew[64]; /* Skew table */
};

struct cpm_dpb dpb_fd = {
	/* SPT */ 26,
	/* BSH */ 3,
	/* BLM */ 7,
	/* EXM */ 0,
	/* DSM */ 242,
	/* DRM */ 63,
	/* AL0 */ 192,
	/* AL1 */ 0,
	/* CKS */ 16,
	/* OFF */ 2,
	/* Skew */ { 0, 6, 12, 18, 24, 4, 10, 16, 22, 2, 8, 14, 20,
	             1, 7, 13, 19, 25, 5, 11, 17, 23, 3, 9, 15, 21 }
};

struct cpm_dpb dpb_hd = {
	/* SPT */ 64,
	/* BSH */ 4,
	/* BLM */ 15,
	/* EXM */ 0,
	/* DSM */ 2441,
	/* DRM */ 1023,
	/* AL0 */ 255,
	/* AL1 */ 255,
	/* CKS */ 0,
	/* OFF */ 2,
	/* Skew */ {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,
	             12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
	             24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
	             36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
	             48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
	             60, 61, 62, 63 }
};

/* Pointer to drive parameter block */
struct cpm_dpb *dpb = &dpb_fd;

/* Starting sector of directory */
#define SECTOR_DIR 0

/* Number of directory sectors */
#define SECTOR_DIR_SIZE ((dpb->drm + 1) * 32 / SECTOR_SIZE)

/* Sectors per block */
#define SECTORS_PER_BLOCK (1 << dpb->bsh)

/* True for a big disk: use two bytes for allocation map entries */
#define BIG_DISK (dpb->dsm > 255)

/* Max sectors for one extent */
#define SECTORS_PER_EXTENT ((BIG_DISK ? 8 : 16) * SECTORS_PER_BLOCK)

/* Blocks per extent */
#define BLOCKS_PER_EXTENT (BIG_DISK ? 8 : 16)

/* Compute extent number of a directory entry */
#define EXTENT_NO(d) (((0x1f & (d)->ex) + 32 * (d)->s2) / (dpb->exm + 1))

/* Compute record count of a directory entry */
#define RC(d) (128 * ((d)->ex & dpb->exm) + (d)->rc)


/* Directory entry size */
#define ENTRY_SIZE 32

/* CP/M 2.2 disk parameter block offsets */

#define CPM_WORD_DPB_SPT 0
#define CPM_BYTE_DPB_BSH 2
#define CPM_BYTE_DPB_BLM 3
#define CPM_BYTE_DPB_EXM 4
#define CPM_WORD_DPB_DSM 5
#define CPM_WORD_DPB_DRM 7
#define CPM_BYTE_DPB_AL0 9
#define CPM_BYTE_DPB_AL1 10
#define CPM_WORD_DPB_CKS 11
#define CPM_WORD_DPB_OFF 13

/* CP/M 2.2 directory entry offsets */

#define CPM_BYTE_DIR_UU 0
#define CPM_BYTE8_DIR_F 1
#define CPM_BYTE3_DIR_T 9
#define CPM_BYTE_DIR_EX 12
#define CPM_BYTE_DIR_S1 13
#define CPM_BYTE_DIR_S2 14
#define CPM_BYTE_DIR_RC 15
#define CPM_BYTE16_DIR_AL 16

struct dirent {
	unsigned char uu;	/* User number: 0xE5 means deleted */
	unsigned char f[8];	/* File name */
	unsigned char t[3];	/* File type t[0].7=readonly, t[1].7=hidden */
	unsigned char ex;	/* Extent counter: 0-31 */
	unsigned char s1;	/* Reserved, set to 0 */
	unsigned char s2;	/* Extent counter high byte */
	unsigned char rc;	/* Number of records in this extent (0x80 for full extent) */
	unsigned char al[16];	/* Allocation map: 0 means free, otherwise block number */
};

/* Extent number is (32*s2 + ex) / (exm+1)
 * Record count is (ex & exm)*128 + rc
 * Basically non-zero exm means extra rc bits are stuffed into the extent number
 * This could be needed to handle large block sizes, where there could be more than
 * 128 records in a single extent.
 */

FILE *disk;

/* Get sector.  Skip reserved tracks.  Deskew. */

void getsect(unsigned char *buf, int sect)
{
	int track = sect / dpb->spt;
	int sector = sect % dpb->spt;
	sector = dpb->skew[sector]; /* De-interleave sectors */
	track += dpb->off; /* Skip over reserved tracks */
        fseek(disk, (track * dpb->spt + sector) * SECTOR_SIZE, SEEK_SET);
        fread((char *)buf, SECTOR_SIZE, 1, disk);
}

void putsect(unsigned char *buf, int sect)
{
	int track = sect / dpb->spt;
	int sector = sect % dpb->spt;
	sector = dpb->skew[sector]; /* De-interleave sectors */
	track += dpb->off; /* Skip over reserved tracks */
        fseek(disk, (track * dpb->spt + sector) * SECTOR_SIZE, SEEK_SET);
        fwrite((char *)buf, SECTOR_SIZE, 1, disk);
}

int lower(int c)
{
        if (c >= 'A' && c <= 'Z')
                return c - 'A' + 'a';
        else
                return c;
}

/* Convert file name from directory into UNIX zero-terminated C string name */

char *getname(struct dirent *d)
{
        static char s[50];
        int p = 0;
        int r;
        int i;
        /* Get name */
        for (i = 0; i != sizeof(d->f); i++) {
                s[p++] = lower(d->f[i]);
        }
        /* Zap trailing spaces */
        while (p && s[p - 1] == ' ') --p;
        /* Append '.' */
        s[p++] = '.';
        r = p;
        /* Get extension */
        for (i = 0; i != sizeof(d->t); i++) {
                s[p++] = lower(d->t[i]);
        }
        /* Zap tailing spaces */
        while (p && s[p - 1] == ' ') --p;
        /* Zap '.' if no extension */
        if (p == r) --p;
        /* Terminate */
        s[p] = 0;
        return s;
}

/* Write UNIX name into Atari directory entry */

void putname(struct dirent *d, char *name)
{
        int x;
        /* Copy file name into directory entry */
        x = 0;
        while (*name && *name != '.' && x < 8) {
                if (*name >= 'a' && *name <= 'z')
                        d->f[x++] = *name++ - 'a' + 'A';
                else
                        d->f[x++] = *name++;
        }
        while (x < 8) {
                d->f[x++] = ' ';
        }
        x = 0;
        while (*name && *name != '.')
                ++name;
        if (*name == '.') {
                ++name;
                while (*name && x < 3) {
                        if (*name >= 'a' && *name <= 'z')
                                d->t[x++] = *name++ - 'a' + 'A';
                        else
                                d->t[x++] = *name++;
                }
        }
        while (x < 3) {
                d->t[x++] = ' ';
        }
}

/* Internal version of name for nice listing */

struct name
{
        char *name;

        /* From directory entry */
        int locked; /* Set if write-protected */
        int sector; /* Starting sector of file */
        int sects; /* Sector count */

        int is_sys; /* Set if it's a .SYS file */
        int is_cm; /* Set if it's a .COM file */

        /* From file itself */
        int load_start;
        int load_size;
        int init;
        int run;
        int size;
};

struct name *names[1024];
int name_n;

int comp(struct name **l, struct name **r)
{
        return strcmp((*l)->name, (*r)->name);
}

/* Return list of free directory entries (extents) for a file */

int alloc_extents(int *list, int extents)
{
        unsigned char buf[SECTOR_SIZE];
        int x;
        for (x = SECTOR_DIR; x != SECTOR_DIR + SECTOR_DIR_SIZE; ++x) {
                int y;
                getsect(buf, x);
                for (y = 0; y != SECTOR_SIZE; y += ENTRY_SIZE) {
                        struct dirent *d = (struct dirent *)(buf + y);
                        if (d->uu == 0xE5) {
                                *list++ = x * (SECTOR_SIZE / ENTRY_SIZE) + (y / ENTRY_SIZE);
                                if (!--extents)
                                        return 0;
                        }
                }
        }
        return -1;
}

/* get allocation map (index by block number) */

unsigned short *alloc_map; /* 0xFFFF is free, 0xFFFE allocated for directory, otherwise entry no. */

void get_map()
{
        unsigned char buf[SECTOR_SIZE];
        int x;
        int entry_no;
        if (!alloc_map) {
                alloc_map = malloc(sizeof(alloc_map[0]) * (dpb->dsm + 1));
                /* Initialize to all free */
                for (x = 0; x != dpb->dsm + 1; ++x)
                        alloc_map[x] = 0xFFFF;
                /* Reserve space for directory */
                for (x = 0; x != (dpb->drm + 1) / ((SECTOR_SIZE << dpb->bsh) / ENTRY_SIZE); ++x)
                        alloc_map[x] = 0xFFFE;
        }
        entry_no = 0;
        for (x = 0; x != SECTOR_DIR_SIZE; ++x) {
                int y;
                /* fprintf(stderr, "sector %d\n", x); */
                getsect(buf, x + SECTOR_DIR);
                for (y = 0; y != SECTOR_SIZE; y += ENTRY_SIZE) {
                        struct dirent *d = (struct dirent *)(buf + y);
                        if (d->uu < 0x20) { /* d->uu != 0xe5 (date stamp is 0x21) */
                                int z;
                                /* 
                                char *s = getname(d);
                                printf("%d %s\n", entry_no, s); */
                                for (z = 0; z != 16; ++z) {
                                        int blk;
                                        if (BIG_DISK) {
                                                blk = d->al[z] + (256 * d->al[z + 1]);
                                                ++z;
                                        } else {
                                                blk = d->al[z];
                                        }
                                        if (blk) {
                                                if (blk >= dpb->dsm + 1) {
                                                        fprintf(stderr, "Entry %d: Found block number (%d) exceeding device size\n", entry_no, blk);
                                                } else if (alloc_map[blk] != 0xFFFF) {
                                                        fprintf(stderr, "Entry %d: Found doubly allocated block number (%d) by entry %d\n", entry_no, blk, alloc_map[blk]);
                                                } else {
                                                        /* Record directory entry number */
                                                        /* printf("setting %d\n", d->al[z]); */
                                                        alloc_map[blk] = entry_no;
                                                }
                                        } else {
                                                break;
                                        }
                                }
                        }
                        ++entry_no;
                }
        }
}

/* Allocate a block.  Returns block number or -1 for out of space.  */

int alloc_block(int entry_no)
{
        int x;
        if (!alloc_map)
                get_map();
        for (x = 0; x != (dpb->dsm + 1); ++x)
                if (alloc_map[x] == 0xFFFF) {
                        alloc_map[x] = entry_no;
                        return x;
                }
        return -1;
}

/* Count free blocks */

int amount_free(void)
{
        int count = 0;
        int x;
        if (!alloc_map)
                get_map();
        for (x = 0; x != (dpb->dsm + 1); ++x)
                if (alloc_map[x] == 0xFFFF) {
                        ++count;
                }
        return count;
}

/* Get directory entry with specific name and extent number
 * (or delete all entries with specified name if del is set)
 * Returns 0 if found, -1 if not found.
 */

int find_file(struct dirent *dir, char *filename, int ex, int del)
{
        unsigned char buf[SECTOR_SIZE];
        int x;
        int flg = -1;
        for (x = 0; x != SECTOR_DIR_SIZE; ++x) {
                int y;
                getsect(buf, x + SECTOR_DIR);
                for (y = 0; y != SECTOR_SIZE; y += ENTRY_SIZE) {
                        struct dirent *d = (struct dirent *)(buf + y);
                        if (d->uu < 0x20 && (del || ex == EXTENT_NO(d))) {
                                char *s = getname(d);
                                if (!strcmp(s, filename)) {
                                        if (del) {
                                                d->uu = 0xe5;
                                                flg = 0;
                                                putsect(buf, x + SECTOR_DIR);
                                        } else {
                                                memcpy(dir, d, ENTRY_SIZE);
                                                flg = 0;
                                                return 0;
                                        }
                                }
                        }
                }
        }
        return flg;
}

/* Read a file: provide with first extent */

int read_file(char *filename, struct dirent *dir, FILE *f)
{
        int rtn = 0;
        unsigned char buf[SECTOR_SIZE];
        int exno = 0; /* Extent number */
        for (;;) {
                int recno; /* Record number within extent */
                for (recno = 0; recno != RC(dir) && recno != SECTORS_PER_EXTENT; ++recno) {
                        int blkno; /* Block number within extent */
                        int recblk; /* Record number within block */
                        int blk; /* Current block */
                        blkno = (recno >> dpb->bsh);
                        recblk = (recno & dpb->blm);
                        if (BIG_DISK)
                                blk = dir->al[blkno * 2] + 256 * dir->al[blkno * 2 + 1];
                        else
                                blk = dir->al[blkno];
                        if (blk) {
                                getsect(buf, (blk << dpb->bsh) + recblk);
                                fwrite(buf, SECTOR_SIZE, 1, f);
                        } else {
                                fprintf(stderr, "allocation map ran out before cr count reached!\n");
                                rtn = -1;
                                break;
                        }
                }
                if (RC(dir) != SECTORS_PER_EXTENT) {
                        break;
                } else {
                        ++exno;
                        if (find_file(dir, filename, exno, 0)) {
                                /* fprintf(stderr, "can't find next extent!\n");
                                rtn = -1; */
                                /* This is normal for case where file is maximum extent size! */
                                break;
                        }
                }
        }
        return rtn;
}

/* cat a file */

void cat(char *name)
{
        struct dirent dir[1];
        if (find_file(dir, name, 0, 0)) {
                printf("File '%s' not found\n", name);
                exit(-1);
        } else {
                /* printf("Found file.  Sector of rib is %d\n", sector); */
                read_file(name, dir, stdout);
        }
}
/* get a file from the disk */

int get_file(char *atari_name, char *local_name)
{
        struct dirent dir[1];
        if (find_file(dir, atari_name, 0, 0)) {
                printf("File '%s' not found\n", atari_name);
                return -1;
        } else {
                FILE *f = fopen(local_name, "w");
                if (!f) {
                        printf("Couldn't open local file '%s'\n", local_name);
                        return -1;
                }
                /* printf("Found file.  Sector of rib is %d\n", sector); */
                read_file(atari_name, dir, f);
                if (fclose(f)) {
                        printf("Couldn't close local file '%s'\n", local_name);
                        return -1;
                }
                return 0;
        }
}

/* Delete file name */

int rm(char *name, int ignore)
{
        struct dirent dir[1];
        if (!find_file(dir, name, 0, 1)) {
                return 0;
        } else {
                if (!ignore)
                        printf("File '%s' not found\n", name);
                return -1;
        }
}

/* Free command */

int do_free(void)
{
        int amount = amount_free() * SECTORS_PER_BLOCK;
        printf("%d free sectors, %d free bytes\n", amount, amount * SECTOR_SIZE);
        return 0;
}

/* Pre-allocate space for file */

int alloc_space(int *list, int blocks)
{
        while (blocks) {
                int x = alloc_block(0xFFFD);
                if (x == -1) {
                        printf("Not enough space\n");
                        return -1;
                } else {
                        *list++ = x;
                }
                --blocks;
        }
        return 0;
}

/* Write a directory entry */

int write_dir(char *name, int extentno, int extent, int rc, int *al)
{
        int z, i;
        struct dirent d[1];
        unsigned char buf[SECTOR_SIZE];
        int sect = extent / (SECTOR_SIZE / ENTRY_SIZE);
        int ofst = extent % (SECTOR_SIZE / ENTRY_SIZE);
        getsect(buf, sect);
        putname(d, name);
        extentno *= (dpb->exm + 1);
        while (rc > 128) {
                rc -= 128;
                ++extentno;
        }
        d->uu = 0;
        d->s1 = 0;
        d->ex = (extentno % 32);
        d->s2 = (extentno / 32);
        d->rc = rc;
        for (i = z = 0; z != 16; ++z) {
                if (BIG_DISK) {
                        d->al[z++] = al[i];
                        d->al[z] = (al[i] >> 8);
                } else {
                        d->al[z] = al[i++];
                }
        }
        /* printf("%s uu=%d s1=%d s2=%d ex=%d rc=%d\n", getname(d), d->uu, d->s1, d->s2, d->ex, d->rc); */
        memcpy(buf + ofst * ENTRY_SIZE, d, ENTRY_SIZE);
        putsect(buf, sect);
        return 0;
}

/* Write a file */

int write_file(char *name, unsigned char *buf, long sects)
{
        int x, z;
        int *blk_list;
        int *extent_list;
        long blks; /* Number of blocks needed for this file */
        long extents; /* Number of extents needed for this file */
        int rc; /* Counter for current extent */
        int secno; /* Sector number within extent */
        int extent_blkno; /* Block number within extent */
        int extentno; /* Extent number of file */
        int blkno; /* Block number */
        int al[16]; /* Allocation map for current extent */


        /* Compute number of blocks needed for file */
        blks = (sects + SECTORS_PER_BLOCK - 1) / SECTORS_PER_BLOCK;

        /* Compute number of extents needed for file */
        extents = ((blks * SECTORS_PER_BLOCK) + SECTORS_PER_EXTENT - 1) / SECTORS_PER_EXTENT;
        /* Force at least one extent for case of empty file */
        if (!extents)
                extents = 1;

        /* Allocate space for file */
        blk_list = (int *)malloc(sizeof(int) * blks);
        if (alloc_space(blk_list, blks))
                return -1;

        /* Allocate extents for file */
        extent_list = (int *)malloc(sizeof(int) * extents);
        if (alloc_extents(extent_list, extents))
                return -1;

        /* Write file */
        for (z = 0; z != 16; ++z)
                al[z] = 0;
        blkno = 0;
        extent_blkno = 0;
        extentno = 0;
        secno = 0;
        rc = 0;
        for (x = 0; x != sects; ++x) {
                al[extent_blkno] = blk_list[blkno];
                putsect(buf + x * SECTOR_SIZE, (blk_list[blkno] << dpb->bsh) + secno);
                ++rc;
                if (++secno == SECTORS_PER_BLOCK) {
                        secno = 0;
                        ++blkno;
                        
                        if (++extent_blkno == BLOCKS_PER_EXTENT) {
                                /* Write current extent */
                                write_dir(name, extentno, extent_list[extentno], rc, al);
                                ++extentno;
                                rc = 0;
                                for (z = 0; z != 16; ++z)
                                        al[z] = 0;
                                extent_blkno = 0;
                        }
                }
        }
        if (rc || !extentno) {
                /* Write final extent */
                write_dir(name, extentno, extent_list[extentno], rc, al);
        }
        
        return 0;
}

/* Put a file on the disk */

int put_file(char *local_name, char *atari_name)
{
        FILE *f = fopen(local_name, "r");
        long x, size; /* File size in bytes */
        long up; /* Size in bytes rounded up to sectors */
        unsigned char *buf;
        int rtn;
        if (!f) {
                printf("Couldn't open '%s'\n", local_name);
                return -1;
        }
        if (fseek(f, 0, SEEK_END)) {
                printf("Couldn't get file size of '%s'\n", local_name);
                fclose(f);
                return -1;
        }
        size = ftell(f);
        if (size < 0)  {
                printf("Couldn't get file size of '%s'\n", local_name);
                fclose(f);
                return -1;
        }
        rewind(f);
        /* Round up to a multiple of (SECTOR_SIZE) */
        up = size + (SECTOR_SIZE) - 1;
        up -= up % (SECTOR_SIZE);

        buf = (unsigned char *)malloc(up);
        if (fread(buf, 1, size, f) != (size_t)size) {
                printf("Couldn't read file '%s'\n", local_name);
                fclose(f);
                free(buf);
                return -1;
        }
        fclose(f);

        /* Fill with ^Zs to end of sector */
        for (x = size; x != up; ++x)
                buf[x] = 0x1a;

        /* Delete existing file */
        rm(atari_name, 1);

        /* Allocate space and write file */
        rtn = write_file(atari_name, buf, up / SECTOR_SIZE);

        if (rtn) {
                printf("Couldn't write file\n");
                return -1;
        }

        return 0;
}

/* Get file size in sectors */

int get_info(struct dirent *dir, char *name)
{
        int rtn = 0;
        int count = 0;
        int exno = 0; /* Extent number */
        /* fprintf(stderr, "info for %s\n", name); */
        for (;;) {
                int recno; /* Record number within extent */
                int cnt = 0;
                /* fprintf(stderr, "extent %d rc=%d\n", exno, dir->rc); */
                for (recno = 0; recno != RC(dir) && recno != SECTORS_PER_EXTENT; ++recno) {
                        int blkno; /* Block number within extent */
                        int blk; /* Current block */
                        blkno = (recno >> dpb->bsh);
                        if (BIG_DISK)
                                blk = dir->al[blkno * 2] + 256 * dir->al[blkno * 2 + 1];
                        else
                                blk = dir->al[blkno];
                        if (blk) {
                                ++count;
                                ++cnt;
                        } else {
                                fprintf(stderr,"allocation map ran out before rc count reached! %d\n", recno);
                                rtn = -1;
                                break;
                        }
                }
                /* fprintf(stderr, "  count=%d\n", cnt); */
                if (RC(dir) != SECTORS_PER_EXTENT) {
                        /* Must be the end */
                        break;
                } else {
                        ++exno;
                        if (find_file(dir, name, exno, 0)) {
                                /* fprintf(stderr, "%s: can't find next extent (%d)!\n", name, exno);
                                rtn = -1; */
                                break;
                        }
                }
        }
        if (rtn)
                return -1;
        else
                return count;
}

void atari_dir(int all, int full, int single)
{
        unsigned char buf[SECTOR_SIZE];
        struct dirent dir[1];
        int x, y;
        int rows;
        int cols = (80 / 13);
        for (x = 0; x != SECTOR_DIR_SIZE; ++x) {
                int y;
                getsect(buf, x + SECTOR_DIR);
                for (y = 0; y != SECTOR_SIZE; y += ENTRY_SIZE) {
                        struct dirent *d = (struct dirent *)(buf + y);
                        if (d->uu < 0x20 && EXTENT_NO(d) == 0) {
                                struct name *nam;
                                char *s = getname(d);
                                nam = (struct name *)malloc(sizeof(struct name));
                                nam->name = strdup(s);
                                if (d->t[0] & 0x80)
                                        nam->locked = 1;
                                else
                                        nam->locked = 0;
                                if (d->t[1] & 0x80)
                                        nam->is_sys = 1;
                                else
                                        nam->is_sys = 0;
                                nam->sector = 0;
                                nam->sects = 0;
                                nam->load_start = -1;
                                nam->load_size = -1;
                                nam->init = -1;
                                nam->run = -1;
                                nam->size = -1;
                                memcpy(dir, d, ENTRY_SIZE);
                                nam->sects = get_info(dir, nam->name);
                                nam->size = nam->sects * SECTOR_SIZE;

                                if ((all || !nam->is_sys))
                                        names[name_n++] = nam;
                        }
                }
        }
        qsort(names, name_n, sizeof(struct name *), (int (*)(const void *, const void *))comp);

        if (full) {
                int totals = 0;
                int total_bytes = 0;
                printf("\n");
                for (x = 0; x != name_n; ++x) {
                        if (names[x]->load_start != -1)
                                printf("-r%c%c%c %6d (%3d) %-13s (load_start=$%x load_end=$%x)\n",
                                       (names[x]->locked ? '-' : 'w'),
                                       (names[x]->is_cm ? 'x' : '-'),
                                       (names[x]->is_sys ? 's' : '-'),
                                       names[x]->size, names[x]->sects, names[x]->name, names[x]->load_start, names[x]->load_start + names[x]->load_size - 1);
                        else
                                printf("-r%c%c%c %6d (%3d) %-13s\n",
                                       (names[x]->locked ? '-' : 'w'),
                                       (names[x]->is_cm ? 'x' : '-'),
                                       (names[x]->is_sys ? 's' : '-'),
                                       names[x]->size, names[x]->sects, names[x]->name);
                        totals += names[x]->sects;
                        total_bytes += names[x]->size;
                }
                printf("\n%d entries\n", name_n);
                printf("\n%d sectors, %d bytes\n", totals, total_bytes);
                printf("\n");
                do_free();
                printf("\n");
        } else if (single) {
                int x;
                for (x = 0; x != name_n; ++x) {
                        printf("%s\n", names[x]->name);
                }
        } else {

                /* Rows of 12 names each ordered like ls */

                rows = (name_n + cols - 1) / cols;

                for (y = 0; y != rows; ++y) {
                        for (x = 0; x != cols; ++x) {
                                int n = y + x * rows;
                                /* printf("%11d  ", n); */
                                if (n < name_n)
                                        printf("%-12s  ", names[n]->name);
                                else
                                        printf("             ");
                        }
                        printf("\n");
                }
        }
}

int mkfs()
{
        unsigned char buf[SECTOR_SIZE];
        int tracks, x, n;
        for (x = 0; x != SECTOR_SIZE; ++x)
	        buf[x] = 0xe5;
        tracks = dpb->off + (((dpb->dsm + 1) << dpb->bsh) + dpb->spt - 1) / dpb->spt;
        n = tracks * dpb->spt;
        printf("%d tracks\n", tracks);
        printf("%d sectors\n", n);
        for (x = 0; x != n; ++x)
                fwrite((char *)buf, SECTOR_SIZE, 1, disk);
        return 0;
}

int main(int argc, char *argv[])
{
        int all = 0;
        int full = 0;
        int single = 0;
	int x;
	long size;
	char *disk_name;
	dpb = &dpb_fd;
	x = 1;
	if (x == argc || !strcmp(argv[x], "--help") || !strcmp(argv[x], "-h")) {
                printf("\nCP/M disk image tool\n");
                printf("\n");
                printf("Syntax: cpmtool path-to-disk-image [command] [args]\n");
                printf("\n");
                printf("  Commands: (default is ls)\n\n");
                printf("      ls [-la1]                     Directory listing\n");
                printf("                  -l for long\n");
                printf("                  -a to show system files\n");
                printf("                  -1 to show a single name per line\n\n");
                printf("      cat cpm-name                  Type file to console\n\n");
                printf("      get cpm-name [local-name]     Copy file from diskette to local-name\n\n");
                printf("      put local-name [cpm-name]     Copy file from local-name to diskette\n\n");
                printf("      free                          Print amount of free space\n\n");
                printf("      rm cpm-name                   Delete a file\n\n");
                printf("      mkfs                          Format disk\n\n");
                return -1;
	}
	disk_name = argv[x++];

	if (x != argc && !strcmp(argv[x], "mkfs")) {
	        disk = fopen(disk_name, "w");
	        if (!disk) {
	                printf("Couldn't open '%s'\n", disk_name);
	                return -1;
	        }
	        mkfs();
	        fclose(disk);
	        return 0;
	}

	disk = fopen(disk_name, "r+");
	if (!disk) {
	        printf("Couldn't open '%s'\n", disk_name);
	        return -1;
	}
	if (fseek(disk, 0, SEEK_END)) {
	        printf("Couldn't seek disk?\n");
	        return -1;
	}
	size = ftell(disk);
	if (size == 128 * 77 * 26) { /* Single sided floppy */
	        dpb = &dpb_fd;
	} else {
	        fprintf(stderr, "assuming hard drive\n");
	        dpb = &dpb_hd;
	}

	/* Directory options */
	dir:
	while (x != argc && argv[x][0] == '-') {
	        int y;
	        for (y = 1;argv[x][y];++y) {
	                int opt = argv[x][y];
	                switch (opt) {
	                        case 'l': full = 1; break;
	                        case 'a': all = 1; break;
	                        case '1': single = 1; break;
	                        default: printf("Unknown option '%c'\n", opt); return -1;
	                }
	        }
	        ++x;
	}

	if (x == argc) {
	        /* Just print a directory listing */
	        atari_dir(all, full, single);
	        return 0;
        } else if (!strcmp(argv[x], "ls")) {
                ++x;
                goto dir;
        } else if (!strcmp(argv[x], "free")) {
                return do_free();
/*        } else if (!strcmp(argv[x], "check")) {
                return do_check(); */
	} else if (!strcmp(argv[x], "cat")) {
	        ++x;
	        if (x == argc) {
	                printf("Missing file name to cat\n");
	                return -1;
	        } else {
	                cat(argv[x++]);
	                return 0;
	        }
	} else if (!strcmp(argv[x], "get")) {
                char *local_name;
                char *atari_name;
                ++x;
                if (x == argc) {
                        printf("Missing file name to get\n");
                        return -1;
                }
                atari_name = argv[x];
                local_name = atari_name;
                if (x + 1 != argc)
                        local_name = argv[++x];
                return get_file(atari_name, local_name);
        } else if (!strcmp(argv[x], "put")) {
                char *local_name;
                char *atari_name;
                ++x;
                if (x == argc) {
                        printf("Missing file name to put\n");
                        return -1;
                }
                local_name = argv[x];
                if (strrchr(local_name, '/'))
                        atari_name = strrchr(local_name, '/') + 1;
                else
                        atari_name = local_name;
                printf("%s\n", atari_name);
                if (x + 1 != argc)
                        atari_name = argv[++x];
                return put_file(local_name, atari_name);
        } else if (!strcmp(argv[x], "rm")) {
                char *name;
                ++x;
                if (x == argc) {
                        printf("Missing name to delete\n");
                        return -1;
                } else {
                        name = argv[x];
                }
                return rm(name, 0);
	} else {
	        printf("Unknown command '%s'\n", argv[x]);
	        return -1;
	}
	return 0;
}
