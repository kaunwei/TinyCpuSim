#include "workqueue.h"

void fw_workqueue_init(fw_workqueue_t *wq) {
    if (!wq) return;
    wq->head = 0;
    wq->tail = 0;
    wq->count = 0;
    wq->drops = 0;
    for (uint32_t i = 0; i < FW_WORKQUEUE_CAPACITY; ++i) {
        wq->items[i].function = (fw_work_fn_t)0;
        wq->items[i].context = (void*)0;
    }
}

int fw_workqueue_schedule_from_isr(fw_workqueue_t *wq, fw_work_fn_t fn, void *ctx) {
    if (!wq || !fn) return -1;
    if (wq->count >= FW_WORKQUEUE_CAPACITY) {
        wq->drops++;
        return -2;
    }
    uint32_t tail = wq->tail;
    wq->items[tail].function = fn;
    wq->items[tail].context = ctx;
    wq->tail = (tail + 1) % FW_WORKQUEUE_CAPACITY;
    wq->count++;
    return 0;
}

uint32_t fw_workqueue_drain(fw_workqueue_t *wq) {
    if (!wq) return 0;
    uint32_t executed = 0;
    while (wq->count > 0) {
        uint32_t head = wq->head;
        fw_work_fn_t fn = wq->items[head].function;
        void *ctx = wq->items[head].context;
        wq->items[head].function = (fw_work_fn_t)0;
        wq->items[head].context = (void*)0;
        wq->head = (head + 1) % FW_WORKQUEUE_CAPACITY;
        wq->count--;
        if (fn) {
            fn(ctx);
            executed++;
        }
    }
    return executed;
}
