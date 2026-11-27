.syntax unified
.arm
.section .text.entry,"ax",%progbits
.balign 4

cp_payload_start:
.macro cp_base reg
    adr \reg, cp_payload_start
    sub \reg, \reg, #0xf000
.endm

.global hook_carplay_send
hook_carplay_send:
    cp_base r3
    b u6_carplay_send

.global hook_carplay_skipped
hook_carplay_skipped:
    push {r0-r3, ip, lr}
    mrs ip, apsr
    push {r4, ip}
    cp_base r2
    bl u6_carplay_skipped
    pop {r4, ip}
    msr apsr_nzcvq, ip
    pop {r0-r3, ip, lr}
    cp_base r3
    add r3, r3, #0x8800
    ldr r3, [r3, #0xb4]
    bx lr
.section .note.GNU-stack,"",%progbits
