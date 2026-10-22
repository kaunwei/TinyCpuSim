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
    .space 14 * 4, 0

.section .text.Reset_Handler, "ax", %progbits
.global Reset_Handler
.type Reset_Handler, %function
Reset_Handler:
    ldr sp, =_stack_top
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
