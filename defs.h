/*-----------------------------------------------------------------------*\
 |  defs.h  --  main definitions for z80 emulator                        |
 |                                                                       |
 |  Copyright 1986-1988 by Parag Patel.  All Rights Reserved.            |
 |  Copyright 1994-1995 by CodeGen, Inc.  All Rights Reserved.           |
\*-----------------------------------------------------------------------*/

#ifndef __DEFS_H_
#define __DEFS_H_

/* the current version of the z80 emulator */
#define VERSION "3.1e"

/* system definitions */
#if defined THINK_C || defined applec || defined macintosh
#	ifndef macintosh
#		define macintosh
#	endif
#elif defined __MWERKS__
#   define BeBox
#elif defined MSDOS && defined GO32
#	define DJGPP
#	ifndef ENDIAN_LITTLE
#		define ENDIAN_LITTLE
#	endif
#else
#	define UNIX	/* cannot use "unix" since DJGPP defines it as well */
#endif


/* some headers define macros this way */
#ifdef BYTE_ORDER
#   if BYTE_ORDER == LITTLE_ENDIAN
#      ifndef ENDIAN_LITTLE
#         define ENDIAN_LITTLE
#      endif
#   else
#      undef ENDIAN_LITTLE
#   endif
#endif

/* misc. handy defs */

#ifndef TRUE
#define TRUE	1
#endif

#ifndef FALSE
#define FALSE	0
#endif

typedef int boolean;

#define CNTL(c) ((c) & 037)	/* convert a char to its control equivalent */

/* handy typedefs for an 8-bit byte, 16-bit word, & 32-bit longword */

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long longword;

/* TODO: Move this to vm.h when the dependency tree is stable */
typedef struct vm_s vm;

#endif /* __DEFS_H_ */
