#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*fw_work_fn_t)(void *context);

typedef struct {
    fw_work_fn_t function;
    void *context;
} fw_work_item_t;

#define FW_WORKQUEUE_CAPACITY 16

typedef struct {
    fw_work_item_t items[FW_WORKQUEUE_CAPACITY];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    volatile uint32_t drops;
} fw_workqueue_t;

void fw_workqueue_init(fw_workqueue_t *wq);

/* Non-blocking: safe to invoke from any ISR (Top Half) */
int fw_workqueue_schedule_from_isr(fw_workqueue_t *wq, fw_work_fn_t fn, void *ctx);

/* Process all queued work items in thread/background context (Bottom Half) */
uint32_t fw_workqueue_drain(fw_workqueue_t *wq);

/* Check if workqueue has pending work items */
static inline int fw_workqueue_has_pending(const fw_workqueue_t *wq) {
    return wq && wq->count > 0;
}

#ifdef __cplusplus
}
#endif
