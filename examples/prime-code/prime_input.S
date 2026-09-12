.syntax unified
.cpu arm926ej-s
.arm
.text

/*
 * Exact privileged byte-copy/cache-flush primitive used by DOOM's
 * install_input_hack()/remove_input_hack().
 *
 * r0 = dst, r1 = src, r2 = byte count
 */
.global prime_privileged_memcpy
.type prime_privileged_memcpy, %function
prime_privileged_memcpy:
    push {r4, r5, r6, lr}
    mrs  r4, apsr
    orr  r5, r4, #128
    msr  cpsr_c, r5

    /* Temporarily grant full domain access, exactly as PureDOOM does. */
    mrc  p15, 0, r6, c3, c0, 0
    mvn  r5, #0
    mcr  p15, 0, r5, c3, c0, 0

1:
    cmp  r2, #0
    beq  2f
    ldrb r3, [r1], #1
    strb r3, [r0], #1
    subs r2, r2, #1
    bne  1b

2:
    /* Same cache/write-buffer maintenance sequence as DOOM. */
    mov  r3, #0
    mcr  p15, 0, r3, c7, c10, 1
    mcr  p15, 0, r3, c7, c5, 0
    mcr  p15, 0, r3, c7, c10, 4

    mcr  p15, 0, r6, c3, c0, 0
    msr  cpsr_c, r4
    pop  {r4, r5, r6, pc}
.size prime_privileged_memcpy, .-prime_privileged_memcpy

/* Prime system calls used by the program. */
.global prime_sys_get_lcd
.type prime_sys_get_lcd, %function
prime_sys_get_lcd:
    str r0, [sp, #-4]!
    str lr, [sp, #-4]!
    svc #0x1008d

.global prime_sys_sleep
.type prime_sys_sleep, %function
prime_sys_sleep:
    str r0, [sp, #-4]!
    str lr, [sp, #-4]!
    svc #0x10008

.global prime_sys_get_event
.type prime_sys_get_event, %function
prime_sys_get_event:
    str r0, [sp, #-4]!
    str lr, [sp, #-4]!
    svc #0x1003f

    /* The SVC return value is kept in r0: 1=event, 0=no event. */

.size prime_sys_get_lcd, .-prime_sys_get_lcd
.size prime_sys_sleep, .-prime_sys_sleep
.size prime_sys_get_event, .-prime_sys_get_event
