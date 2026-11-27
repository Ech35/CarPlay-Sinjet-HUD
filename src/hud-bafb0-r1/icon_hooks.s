// HUDBAFB0 20201012. Append-only code; ordered entry veneers.
    b.w draw_icon
    b.w logical_picture
    b.w format_picture

// All four original theme tables share navigation group 5 and its 68px style.
draw_icon:
    sub.w r3, r0, #40
    cmp r3, #45
    bhi draw_stock
    push {r4, r5, r6, lr}
    mov r4, r0
    add.w r6, r3, #0x4000
    bl #0x08086ee0
    mov r5, r0
    mov r3, r6
    b.w #0x080903aa
draw_stock:
    push {r4, r5, r6, lr}
    mov r4, r0
    b.w #0x08090398

// Reuse the stock dynamic picture descriptor, without changing the GUI setter.
// Return (logical, normal, pressed, disabled), each the extended physical ID.
logical_picture:
    sub.w r1, r0, #0x4000
    cmp r1, #45
    bhi logical_stock
    ldr r3, dynamic_descriptor
    strh r0, [r3]
    strh r0, [r3, #2]
    strh r0, [r3, #4]
    strh r0, [r3, #6]
    mov r0, r3
    bx lr
logical_stock:
    mov r1, r0
    movs r2, #0
    b.w #0x08060bfc

// New names are bounded, unlike the unchanged stock literal-name path.
format_picture:
    sub.w r3, r0, #0x4000
    cmp r3, #45
    bhi format_stock
    push {r4, lr}
    adds r3, #40
    adr.w r4, standard_format
    mov r0, r1
    mov r1, r2
    mov r2, r4
    mov r4, r1
    bl #0x08044df0
    cmp r0, r4
    ite lo
    movlo r0, #1
    movhs r0, #0
    pop {r4, pc}
format_stock:
    push.w {r4, r5, r6, r7, r8, lr}
    b.w #0x080653bc

    .balign 4
dynamic_descriptor:
    .word 0x20000088
standard_format:
    .asciz "naviInfo.pak>turnArrow_%d.png"
    .balign 4
