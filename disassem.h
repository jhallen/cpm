#ifndef __DISASSEM_H_
#define __DISASSEM_H_

#include "z80.h"

int disassemlen(void);
int disassem(z80info *z80, word start, FILE *fp);

#endif /* __DISASSEM_H_ */
