#include "../bsp/platform.h"
#include "../bsp/uart.h"
#include "../bsp/timer.h"
#include "../bsp/scheduler.h"

static fw_scheduler_t g_sched;

volatile uint32_t g_count_1ms = 0;
volatile uint32_t g_count_10ms = 0;
volatile uint32_t g_count_100ms = 0;
volatile uint32_t g_count_heartbeat = 0;
volatile uint32_t g_service_runs = 0;
volatile uint32_t g_last_executed = 0; // 1 = timer callback, 2 = service

void Timer_IRQHandler(void) {
    TIMER0->INTCLR = 1U;
    fw_scheduler_on_timer_tick(&g_sched);
}

static void cb_1ms(void *ctx) {
    (void)ctx;
    g_count_1ms++;
    g_last_executed = 1;
}

static void cb_10ms(void *ctx) {
    (void)ctx;
    g_count_10ms++;
    g_last_executed = 1;
}

static void cb_100ms(void *ctx) {
    (void)ctx;
    g_count_100ms++;
    g_last_executed = 1;
}

static void cb_heartbeat(void *ctx) {
    (void)ctx;
    g_count_heartbeat++;
    g_last_executed = 1;
    uart_puts("[HB]\n");
}

static void ordinary_service(void *ctx) {
    (void)ctx;
    g_service_runs++;
    g_last_executed = 2;
}

int main(void) {
    uart_init(1U);
    fw_scheduler_init(&g_sched);

    fw_scheduler_add_timer_task(&g_sched, 1, cb_1ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 10, cb_10ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 100, cb_100ms, (void*)0);
    fw_scheduler_add_timer_task(&g_sched, 1000, cb_heartbeat, (void*)0);
    fw_scheduler_add_service(&g_sched, ordinary_service, (void*)0);

    /* Configure timer for 1 ms tick (at 1 MHz simulated clock, 1 ms = 1000 ticks) */
    /* For fast test execution, 100 ticks per tick */
    NVIC->ISER = (1u << 1);
    timer_init(100U, 1U);

    uart_puts("SCHED_START\n");

    while (g_count_heartbeat < 3) {
        fw_scheduler_run_iteration(&g_sched);
    }

    uart_puts("SCHEDULER_ACCEPTANCE_PASS\n");
    return 0;
}
