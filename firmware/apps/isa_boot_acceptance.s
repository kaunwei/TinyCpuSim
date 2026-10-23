.syntax unified
.cpu cortex-m3
.thumb

.section .text.exercise_isa_families,"ax",%progbits
.global exercise_isa_families
.type exercise_isa_families,%function
exercise_isa_families:
    push {r4-r7, lr}
    sub sp, #16
    add r4, sp, #4
    adr r5, .Laddress
    movs r2, #0x80
    sxtb r2, r2
    sxth r2, r2
    uxtb r2, r2
    uxth r2, r2
    ldr r3, .Lword
    rev r3, r3
    rev16 r3, r3
    revsh r3, r3
    movs r0, #100
    movs r1, #5
    udiv r0, r0, r1
    movs r6, #100
    sdiv r6, r6, r1
    push.w {r4, r8, lr}
    pop.w {r4, r8, lr}
    cmp r0, #20
    beq.w .Lwide_taken
    movs r0, #0
.Lwide_taken:
    dmb
    dsb
    isb
    cpsid i
    cpsie i
    add sp, #16
    movw r0, #0xACE0
    movt r0, #0x2468
    pop {r4-r7, pc}
.align 2
.Laddress:
    .word 0
.Lword:
    .word 0x12345678
.size exercise_isa_families, .-exercise_isa_families
