#include "scheduler.h"

void fw_scheduler_init(fw_scheduler_t *sched) {
    if (!sched) return;
    sched->num_timer_tasks = 0;
    sched->num_services = 0;
    sched->tick_count_ms = 0;
    sched->timer_first_order_violations = 0;
    for (uint32_t i = 0; i < FW_TIMER_TASK_CAPACITY; ++i) {
        sched->timer_tasks[i].period_ms = 0;
        sched->timer_tasks[i].elapsed_ms = 0;
        sched->timer_tasks[i].callback = (void*)0;
        sched->timer_tasks[i].context = (void*)0;
        sched->timer_tasks[i].reserved = 0;
        sched->pending[i] = 0;
#ifdef DEBUG
        sched->diags[i].overrun_count = 0;
        sched->diags[i].execution_count = 0;
        sched->diags[i].max_delay_ticks = 0;
#endif
    }
    for (uint32_t i = 0; i < FW_MAIN_SERVICE_CAPACITY; ++i) {
        sched->services[i].service = (void*)0;
        sched->services[i].context = (void*)0;
    }
}

int fw_scheduler_add_timer_task(fw_scheduler_t *sched, uint16_t period_ms, fw_task_fn_t callback, void *context) {
    if (!sched || !callback || period_ms == 0) return -1;
    if (sched->num_timer_tasks >= FW_TIMER_TASK_CAPACITY) return -1;
    uint32_t idx = sched->num_timer_tasks++;
    sched->timer_tasks[idx].period_ms = period_ms;
    sched->timer_tasks[idx].elapsed_ms = 0;
    sched->timer_tasks[idx].callback = callback;
    sched->timer_tasks[idx].context = context;
    sched->timer_tasks[idx].reserved = 0;
    sched->pending[idx] = 0;
    return (int)idx;
}

int fw_scheduler_add_service(fw_scheduler_t *sched, fw_task_fn_t service, void *context) {
    if (!sched || !service) return -1;
    if (sched->num_services >= FW_MAIN_SERVICE_CAPACITY) return -1;
    uint32_t idx = sched->num_services++;
    sched->services[idx].service = service;
    sched->services[idx].context = context;
    return (int)idx;
}

void fw_scheduler_on_timer_tick(fw_scheduler_t *sched) {
    if (!sched) return;
    sched->tick_count_ms++;
    for (uint32_t i = 0; i < sched->num_timer_tasks; ++i) {
        fw_timer_task_t *task = &sched->timer_tasks[i];
        task->elapsed_ms++;
        if (task->elapsed_ms >= task->period_ms) {
            task->elapsed_ms = 0;
            if (sched->pending[i] != 0) {
#ifdef DEBUG
                sched->diags[i].overrun_count++;
#endif
            } else {
                sched->pending[i] = 1;
            }
        }
    }
}

void fw_scheduler_run_iteration(fw_scheduler_t *sched) {
    if (!sched) return;

    /* 1. Timer callbacks FIRST: handle all pending timer tasks before ordinary services */
    for (uint32_t i = 0; i < sched->num_timer_tasks; ++i) {
        if (sched->pending[i] != 0) {
            sched->pending[i] = 0;
#ifdef DEBUG
            sched->diags[i].execution_count++;
#endif
            fw_task_fn_t cb = sched->timer_tasks[i].callback;
            if (cb) {
                cb(sched->timer_tasks[i].context);
            }
        }
    }

    /* 2. Check for order violation: no timer task should become pending and be bypassed by services */
    for (uint32_t i = 0; i < sched->num_timer_tasks; ++i) {
        if (sched->pending[i] != 0) {
            sched->timer_first_order_violations++;
            break;
        }
    }

    /* 3. Ordinary services SECOND */
    for (uint32_t i = 0; i < sched->num_services; ++i) {
        fw_task_fn_t svc = sched->services[i].service;
        if (svc) {
            svc(sched->services[i].context);
        }
    }
}
