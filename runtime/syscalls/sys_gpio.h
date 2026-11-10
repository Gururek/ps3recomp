/*
 * ps3recomp - Retail LV2 GPIO syscalls
 */

#ifndef SYS_GPIO_H
#define SYS_GPIO_H

#include "lv2_syscall_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_GPIO_LED_DEVICE_ID        1u
#define SYS_GPIO_DIP_SWITCH_DEVICE_ID 2u

int64_t sys_gpio_get(ppu_context* ctx);
int64_t sys_gpio_set(ppu_context* ctx);
void sys_gpio_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_GPIO_H */
