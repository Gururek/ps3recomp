/*
 * ps3recomp - Retail LV2 GPIO syscalls
 */

#include "sys_gpio.h"
#include "../memory/vm.h"

#include <string.h>

int64_t sys_gpio_get(ppu_context* ctx)
{
    uint64_t device_id = LV2_ARG_U64(ctx, 0);
    uint32_t value_out = LV2_ARG_PTR(ctx, 1);

    if (device_id != SYS_GPIO_LED_DEVICE_ID &&
        device_id != SYS_GPIO_DIP_SWITCH_DEVICE_ID)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (value_out == 0 || !VM_READY ||
        (ppu_vm_size && (uint64_t)value_out + sizeof(uint64_t) > ppu_vm_size))
        return (int64_t)(int32_t)CELL_EFAULT;

    /* Retail consoles expose neither developer LEDs nor DIP switches. */
    memset(vm_to_host(value_out), 0, sizeof(uint64_t));
    return CELL_OK;
}

int64_t sys_gpio_set(ppu_context* ctx)
{
    uint64_t device_id = LV2_ARG_U64(ctx, 0);

    if (device_id == SYS_GPIO_LED_DEVICE_ID)
        return CELL_OK;
    if (device_id == SYS_GPIO_DIP_SWITCH_DEVICE_ID)
        return (int64_t)(int32_t)CELL_EINVAL;
    return (int64_t)(int32_t)CELL_ESRCH;
}

void sys_gpio_init(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, SYS_GPIO_SET, sys_gpio_set);
    lv2_syscall_register(tbl, SYS_GPIO_GET, sys_gpio_get);
}
