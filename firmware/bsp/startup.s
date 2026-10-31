.syntax unified
.cpu cortex-m3
.thumb

.section .vectors, "a", %progbits
.global g_pfnVectors
g_pfnVectors:
    .word _stack_top
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word 0
    .word 0
    .word 0
    .word SysTick_Handler
    /* External Interrupts (16+) */
    .word UART_IRQHandler
    .word Timer_IRQHandler
    .word GPIO_IRQHandler
    .word DMA_IRQHandler
    .word Mailbox_IRQHandler
    .space 11 * 4, 0

.section .text.Reset_Handler, "ax", %progbits
.global Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    ldr sp, =_stack_top

    /* Copy .data section from FLASH (_etext) to RAM (_sdata .. _edata) */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_etext
1:
    cmp r0, r1
    bhs 2f
    ldr r3, [r2], #4
    str r3, [r0], #4
    b 1b

2:
    /* Zero .bss section (_sbss .. _ebss) */
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
3:
    cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b

4:
    bl main
    /* On return from main: clean exit via SVC #0 */
    svc #0
    b .

.global Default_Handler
Default_Handler:
    bx lr

.weak NMI_Handler
.set NMI_Handler, Default_Handler
.weak HardFault_Handler
.set HardFault_Handler, Default_Handler
.weak SVC_Handler
.set SVC_Handler, Default_Handler
.weak SysTick_Handler
.set SysTick_Handler, Default_Handler
.weak UART_IRQHandler
.set UART_IRQHandler, Default_Handler
.weak Timer_IRQHandler
.set Timer_IRQHandler, Default_Handler
.weak GPIO_IRQHandler
.set GPIO_IRQHandler, Default_Handler
.weak DMA_IRQHandler
.set DMA_IRQHandler, Default_Handler
.weak Mailbox_IRQHandler
.set Mailbox_IRQHandler, Default_Handler
