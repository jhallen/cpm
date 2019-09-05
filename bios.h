#ifndef __BIOS_H_
#define __BIOS_H_

#include "defs.h"
#include "z80.h"

/* bios.c class */
typedef struct bios_s bios;

bios *bios_new(vm *vm);
void bios_set_silent_exit(bios *obj, int silent_exit);
void bios_call(bios *obj, z80info *z80, unsigned int fn);
void bios_sysreset(bios *obj, z80info *z80);
void bios_warmboot(bios *obj, z80info *z80);
void bios_finish(bios *obj, z80info *z80);
void bios_destroy(bios *obj);

#endif /* __BIOS_H_ */
