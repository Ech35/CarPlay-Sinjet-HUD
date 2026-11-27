.syntax unified
.arm
.section .text.entry,"ax",%progbits
.balign 4

payload_start:
.macro base_for reg
    adr \reg, payload_start
    sub \reg, \reg, #0x21000
.endm

.macro save_volatile
    push {r0-r3, ip, lr}
    mrs ip, apsr
    push {r4, ip}
.endm
.macro restore_volatile
    pop {r4, ip}
    msr apsr_nzcvq, ip
    pop {r0-r3, ip, lr}
.endm

.global hook_ident
hook_ident:
    save_volatile
    mov r0, r5
    mov r1, r8
    mov r2, r6
    base_for r3
    bl u6_ident
    restore_volatile
    mov r0, r6
    bx lr

.macro incoming target
    save_volatile
    sub sp, sp, #8
    base_for ip
    str ip, [sp]
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, r7
    bl \target
    add sp, sp, #8
    restore_volatile
.endm

.global hook_before
hook_before:
    incoming u6_before
    cmp r7, #1
    bx lr

.global hook_after
hook_after:
    incoming u6_after
    ldr r3, [sp, #4]
    bx lr

.global hook_closing
hook_closing:
    save_volatile
    base_for r1
    bl u6_closing
    restore_volatile
    push {r4-r6,lr}
    base_for ip
    add ip, ip, #0x11000
    add ip, ip, #0xd00
    add ip, ip, #8
    bx ip

.global hook_auth
hook_auth:
    save_volatile
    mov r0, r6
    base_for r1
    bl u6_auth
    @ r4 is otherwise unused on this success tail. The stock epilogue restores it.
    str r0, [sp]
    restore_volatile
    mov r0, #3
    bx lr

.global hook_auth_complete
hook_auth_complete:
    save_volatile
    @ The state-4 failure callback also reaches this epilogue with nonzero r5.
    cmp r5, #0
    bne 1f
    mov r0, r4
    base_for r1
    bl u6_auth_complete
1:
    restore_volatile
    mov r0, #0
    bx lr

.global hook_control_handoff
hook_control_handoff:
    save_volatile
    sub sp, sp, #8
    base_for ip
    str ip, [sp]
    mov r0, r4
    mov r1, r7
    mov r2, r6
    mov r3, r8
    bl u6_control_handoff
    add sp, sp, #8
    restore_volatile
    base_for ip
    add ip, ip, #0xd000
    add ip, ip, #0x700
    add ip, ip, #0x6c
    bx ip

.global hook_service
hook_service:
    save_volatile
    mov r0, r4
    base_for r1
    bl u6_service
    restore_volatile
    ldr r0, [r4, #0xc]
    bx lr

.global hook_wait
hook_wait:
    push {r4, lr}
    mov r2, r4
    base_for r3
    bl u6_wait
    cmn r0, #1
    pop {r4, lr}
    bxne lr
    mov r0, r5
    base_for ip
    add ip, ip, #0x4800
    add ip, ip, #8
    bx ip
.section .note.GNU-stack,"",%progbits
