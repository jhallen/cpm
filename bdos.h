#ifndef __BDOS_H_
#define __BDOS_H_

/* bdos class */

typedef struct bdos_s bdos;

#define BDOS_HOOK 0xDC06

bdos *bdos_new(vm *vm, bios *bios);
void bdos_set_cmd(bdos *obj, char *cmd);
void bdos_set_exec(bdos *obj, int exec);
void bdos_set_trace_bdos(bdos *obj, int trace_bdos);
void bdos_check_hook(bdos *obj, z80info *z80);
char *bdos_decode(int n);
int bdos_fcb(int n);
void bdos_fcb_dump(z80info *z80);
void bdos_destroy(bdos *obj);

#endif /* __BDOS_H_ */
