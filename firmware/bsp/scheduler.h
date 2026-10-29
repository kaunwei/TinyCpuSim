#ifndef FW_SCHEDULER_H
#define FW_SCHEDULER_H

#include <stdint.h>
#include <stddef.h>
#include "../template/fw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fw_task_fn_t)(void *context);

typedef struct {
    uint16_t period_ms;
    uint16_t elapsed_ms;
    fw_task_fn_t callback;
    void *context;
    uint32_t reserved;
} fw_timer_task_t;

typedef struct {
    fw_task_fn_t service;
    void *context;
} fw_main_service_t;

#ifdef DEBUG
typedef struct {
    uint32_t overrun_count;
    uint32_t execution_count;
    uint32_t max_delay_ticks;
} fw_task_diag_t;
#endif

typedef struct {
    fw_timer_task_t timer_tasks[FW_TIMER_TASK_CAPACITY];
    uint32_t num_timer_tasks;
    fw_main_service_t services[FW_MAIN_SERVICE_CAPACITY];
    uint32_t num_services;
    volatile uint8_t pending[FW_TIMER_TASK_CAPACITY];
    volatile uint32_t tick_count_ms;
    volatile uint32_t timer_first_order_violations;
#ifdef DEBUG
    fw_task_diag_t diags[FW_TIMER_TASK_CAPACITY];
#endif
} fw_scheduler_t;

void fw_scheduler_init(fw_scheduler_t *sched);
int fw_scheduler_add_timer_task(fw_scheduler_t *sched, uint16_t period_ms, fw_task_fn_t callback, void *context);
int fw_scheduler_add_service(fw_scheduler_t *sched, fw_task_fn_t service, void *context);

/* Called strictly from Timer ISR: sets pending flags only, never invokes callbacks */
void fw_scheduler_on_timer_tick(fw_scheduler_t *sched);

/* Called from main loop: executes pending timer tasks first, then ordinary services */
void fw_scheduler_run_iteration(fw_scheduler_t *sched);

#ifdef __cplusplus
}
#endif

#endif /* FW_SCHEDULER_H */
