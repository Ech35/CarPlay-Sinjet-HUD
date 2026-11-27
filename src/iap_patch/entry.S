    .syntax unified
    .arm
    .section .text.entry, "ax", %progbits

    .macro loadbase dst, tmp, anchor
    adr     \dst, \anchor
    movw    \tmp, #:lower16:\anchor
    movt    \tmp, #:upper16:\anchor
    sub     \dst, \dst, \tmp
    .endm

    .global probe_outbound
probe_outbound:
    cmp     r6, #0x1d00
    sub     r3, r6, #0x1d00
    cmp     r3, #1
    bne     1f
    push    {r0-r3, r12, lr}
    mov     r0, r5
    mov     r1, r6
    mov     r2, r11
    loadbase r3, r12, probe_outbound
    bl      identify_probe
    pop     {r0-r3, r12, lr}
1:  ldr     r3, [r11, #4]
    bx      lr

    .global probe_accepted
probe_accepted:
    push    {r0-r3, r12, lr}
    mov     r0, r5
    mov     r1, r7
    loadbase r2, r12, probe_accepted
    bl      accepted_probe
    pop     {r0-r3, r12, lr}
    ldr     r3, [r7]
    bx      lr

    .global probe_received
probe_received:
    push    {r0-r3, r12, lr}
    mov     r0, r4
    mov     r1, r5
    mov     r2, r6
    loadbase r12, r3, probe_received
    sub     sp, sp, #8
    str     r12, [sp]
    mov     r3, r7
    bl      received_probe
    add     sp, sp, #8
    cmp     r0, #0
    pop     {r0-r3, r12, lr}
    ldrb    r3, [r4, #0x154]
    bxeq    lr
    loadbase r12, r3, probe_received
    movw    r3, #0xd4a4
    add     pc, r12, r3

    .global probe_ready
probe_ready:
    push    {r0-r3, r12, lr}
    mov     r0, r5
    loadbase r1, r12, probe_ready
    bl      ready_probe
    pop     {r0-r3, r12, lr}
    mov     r0, r6
    bx      lr

    .global probe_transport
probe_transport:
    push    {r0-r3, r12, lr}
    loadbase r12, r3, probe_transport
    sub     sp, sp, #8
    str     r12, [sp]
    ldr     r3, [sp, #20]
    bl      transport_probe
    add     sp, sp, #8
    loadbase r12, r3, probe_transport
    movw    r3, #0xcd94
    add     r12, r12, r3
    str     r12, [sp, #16]
    pop     {r0-r3, r12, lr}
    /* Reproduce the original prologue with the caller's LR intact. */
    push    {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    bx      r12

    .global probe_accepted_return
probe_accepted_return:
    push    {r0-r3, r12, lr}
    mov     r0, r5
    loadbase r1, r12, probe_accepted_return
    bl      accepted_return_probe
    pop     {r0-r3, r12, lr}
    mov     r0, r6
    bx      lr

    .global probe_pass_applied
probe_pass_applied:
    push    {r0-r3, r12, lr}
    mov     r0, r4
    loadbase r1, r12, probe_pass_applied
    bl      pass_applied_probe
    pop     {r0-r3, r12, lr}
    add     sp, sp, #8
    bx      lr

    .global probe_pass_state
probe_pass_state:
    push    {r0-r3, r12, lr}
    loadbase r3, r12, probe_pass_state
    bl      pass_state_probe
    pop     {r0-r3, r12, lr}
    str     r2, [r0, #0x8c]
    bx      lr

    .global probe_hu_location
probe_hu_location:
    push    {r0-r3, r12, lr}
    loadbase r3, r12, probe_hu_location
    bl      hu_location_probe
    pop     {r0-r3, r12, lr}
    loadbase r12, r3, probe_hu_location
    movw    r3, #0xd358
    add     r12, r12, r3
    ldr     r3, [r0]
    bx      r12
