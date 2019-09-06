#ifndef __VM_H_
#define __VM_H_

#include "z80.h"
#include "defs.h"

/* main.c contains vm class */

typedef struct vm_s vm;

vm *vm_new();
void vm_resetterm(vm *obj);
void vm_setterm(vm *obj);
boolean vm_input(vm *obj, z80info *z80, byte haddr, byte laddr, byte *val);
void vm_output(vm *obj, z80info *z80, byte haddr, byte laddr, byte data);
void vm_haltcpu(vm *obj, z80info *z80);
word vm_read_mem(vm *obj, z80info *z80, word addr);
word vm_write_mem(vm *obj, z80info *z80, word addr, byte val);
void vm_undefinstr(vm *obj, z80info *z80, byte instr);
boolean vm_loadfile(z80info *z80, const char *fname);
void vm_destroy(vm *obj);

#endif /* __VM_H_ */
