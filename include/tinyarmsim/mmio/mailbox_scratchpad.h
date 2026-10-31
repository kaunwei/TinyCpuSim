#ifndef TINYARMSIM_MMIO_MAILBOX_SCRATCHPAD_H
#define TINYARMSIM_MMIO_MAILBOX_SCRATCHPAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mailbox Register Offsets */
#define MB_REG_STATUS              0x00
#define MB_REG_PARAM_WIDTH         0x04
#define MB_REG_PARAM_HEIGHT        0x08
#define MB_REG_PARAM_CHANNELS      0x0C
#define MB_REG_PARAM_KERNEL_SZ     0x10
#define MB_REG_DOORBELL_TO_ACCEL   0x14
#define MB_REG_DOORBELL_TO_HOST    0x18
#define MB_REG_INTCLR              0x1C

/* Status Bitmask Definitions */
#define MB_STATUS_IDLE             0x00
#define MB_STATUS_RUNNING          0x01
#define MB_STATUS_DONE             0x02
#define MB_STATUS_ERROR            0x03

/* Scratchpad Memory Aperture */
#define MB_SCRATCHPAD_OFFSET       0x1000
#define MB_SCRATCHPAD_SIZE         (64 * 1024)

#ifdef __cplusplus
}
#endif

#endif /* TINYARMSIM_MMIO_MAILBOX_SCRATCHPAD_H */
