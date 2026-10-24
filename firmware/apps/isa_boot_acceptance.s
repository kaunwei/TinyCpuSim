.syntax unified
.cpu cortex-m3
.thumb

.section .text.exercise_isa_families,"ax",%progbits
.global exercise_isa_families
.type exercise_isa_families,%function
// void exercise_isa_families(uint32_t value, struct AcceptanceResults *out)
// r0 is the C input and r1 points to the 13-word result record.
exercise_isa_families:
    push.w {r4-r8,lr}
    mov r4,r1
    mov r7,sp
    sub sp,#16
    add r5,sp,#4
    subs r5,r7,r5
    str r5,[r4,#0]
    adr r6,isa_address_marker
    str r6,[r4,#4]

    movs r2,#0x80
    sxtb r2,r2
    str r2,[r4,#8]
    sxth r2,r2
    str r2,[r4,#12]
    uxtb r2,r2
    str r2,[r4,#16]
    uxth r2,r2
    str r2,[r4,#20]

    ldr r3,.Lword
    rev r6,r3
    str r6,[r4,#24]
    rev16 r6,r3
    str r6,[r4,#28]
    revsh r6,r3
    str r6,[r4,#32]

    movs r0,#100
    movs r1,#5
    udiv r0,r0,r1
    str r0,[r4,#36]
    ldr r6,.Lnegative
    movs r7,#5
    sdiv r6,r6,r7
    str r6,[r4,#40]

    movs r2,#0x55
    mov r8,r2
    push.w {r5,r8,lr}
    movs r2,#0
    mov r8,r2
    pop.w {r5,r8,lr}
    movs r2,#0x55
    cmp r8,r2
    beq.w .Lstack_equal
    movs r2,#0
    b .Lstack_done
.Lstack_equal:
    movs r2,#1
.Lstack_done:
    str r2,[r4,#44]

    cmp r0,#20
    beq.w .Lwide_taken
    movs r2,#0
    b .Lwide_done
.Lwide_taken:
    movs r2,#1
.Lwide_done:
    str r2,[r4,#48]

    dmb
    dsb
    isb
    cpsid i
    cpsie i
    add sp,#16
    pop.w {r4-r8,pc}
.align 2
.global isa_address_marker
isa_address_marker:
    .word 0
.Lword:
    .word 0x12345678
.Lnegative:
    .word 0xFFFFFF80
.size exercise_isa_families,.-exercise_isa_families
