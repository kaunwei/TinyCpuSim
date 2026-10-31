#include "platform.h"
#include "uart.h"
#include "timer.h"
#include "scheduler.h"
#include "shell.h"

static fw_scheduler_t g_sched;
static fw_shell_t g_shell;

volatile uint32_t g_count_1ms = 0;
volatile uint32_t g_count_10ms = 0;
volatile uint32_t g_count_100ms = 0;
volatile uint32_t g_count_heartbeat = 0;
volatile uint32_t g_bg_runs = 0;

void Timer_IRQHandler(void) {
    TIMER0->INTCLR = 1U;
    fw_scheduler_on_timer_tick(&g_sched);
}

static void cb_1ms(void *ctx) {
    (void)ctx;
    g_count_1ms++;
}

static void cb_10ms(void *ctx) {
    (void)ctx;
    g_count_10ms++;
}

static void cb_100ms(void *ctx) {
    (void)ctx;
    g_count_100ms++;
}

static void cb_heartbeat(void *ctx) {
    (void)ctx;
    g_count_heartbeat++;
    uart_puts("[HB]\n");
}

static void bg_service(void *ctx) {
    (void)ctx;
    g_bg_runs++;
}

int main(void) {
    uart_init(1U);
    fw_scheduler_init(&g_sched);
    fw_shell_init(&g_shell);

    fw_scheduler_add_timer_task(&g_sched, 1, cb_1ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 10, cb_10ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 100, cb_100ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 1000, cb_heartbeat, (void*)0);

    fw_scheduler_add_service(&g_sched, fw_shell_service, &g_shell);
    fw_scheduler_add_service(&g_sched, bg_service, (void*)0);

    NVIC->ISER = (1u << 1);
    timer_init(100U, 1U);

    uart_puts("REFERENCE_FIRMWARE_START\n");

    while (1) {
        fw_scheduler_run_iteration(&g_sched);
    }

    return 0;
}
