# MMIO Peripheral Timing and Performance Modeling Dossier

## Executive Summary
This dossier explores the peripheral timing, clocking, and performance modeling mechanisms across TinyCpuSim (`include/tinyarmsim/mmio/`, `memory_bus.hpp`, `interpreter.hpp`, `ooo_core.hpp`, and `tests/baremetal_firmware_test.cpp`). Current MMIO devices are strictly **functional-only (0-cycle immediate)** models. Bus transactions incur zero latency, zero wait states, and no bus stalls. Furthermore, `MemoryBus::tick(cycles)` is **completely disconnected** from both CPU execution engines (`IsaInterpreter` and `OOOCore`), leaving peripheral clocks un-driven and firmware polling loops vulnerable to infinite hangs or unrealistic 0-cycle fall-throughs.

---

## 1. Functional vs. Performance Modeling Assessment
All MMIO peripherals implement 0-cycle instantaneous functional models:

| Peripheral | Base Address | Interface File & Seams | Timing & Latency Characteristics |
| :--- | :--- | :--- | :--- |
| **MMIODevice** | Virtual base | `include/tinyarmsim/mmio/device.hpp#L14-L30` | `read()` / `write()` are pure virtual synchronous calls (`#L21-L22`). `tick()` is a no-op default (`#L24`). No latency or wait-state return. |
| **UartDevice** | `0x40000000` | `include/tinyarmsim/mmio/uart.hpp#L10-L87` | Functional-only. Does not override `tick()`. Status register `0x04` unconditionally returns `TX_READY` (bit 0) and `TX_EMPTY` (bit 2) (`#L34`). Writes append to `tx_output_` instantly (`#L48`). |
| **TimerDevice** | `0x40001000` | `include/tinyarmsim/mmio/timer.hpp#L9-L80` | Overrides `tick(cycles)` (`#L44-L60`), decrementing `value_` directly (`#L58`). However, lacks prescaler and bus latency modeling. |
| **I2CDevice** | `0x40002000` | `include/tinyarmsim/mmio/i2c.hpp#L11-L116` | Functional-only. Does not override `tick()`. Transfers complete synchronously in `execute_control()` (`#L67-L86`) and `write_data_byte()` (`#L88-L105`). `TXE`/`ACK` asserted immediately. |
| **SpiDevice** | `0x40003000` | `include/tinyarmsim/mmio/spi.hpp#L10-L91` | Functional-only. Does not override `tick()`. Flash command decoding and byte transfer occur instantly inside `transfer_byte()` (`#L57-L80`). `RXNE`/`TXE` set in 0 cycles (`#L78-L79`). |
| **Crc32Device**| `0x40004000` | `include/tinyarmsim/mmio/crc32.hpp#L10-L79` | Functional-only. CRC polynomial calculation is computed immediately on write (`#L30-L52`). |
| **CustomDevice**| `0x50000000`| `include/tinyarmsim/mmio/custom_device.hpp#L10-L60` | Functional-only. Executes ADD/MUL/XOR immediately upon command register write (`#L38, L51-L60`). |
| **MemoryBus** | System Bus | `include/tinyarmsim/memory_bus.hpp#L14-L214` | Dispatches `read8/16/32` and `write8/16/32` directly to slot device (`#L101, L113, L127, L142, L156, L171`) with 0 wait states, 0 bus arbitration delay. |

---

## 2. Bus Tick Driving Topology

| Context | Call Site Pointers | Analysis & Disconnection Proof |
| :--- | :--- | :--- |
| **IsaInterpreter::step()** | `include/tinyarmsim/interpreter.hpp#L608-L668` | **Not driven**. Lines 611-615 poll `has_pending_irq()` to assert NVIC lines, but `bus_.tick()` is never called. |
| **IsaInterpreter::run()** | `include/tinyarmsim/interpreter.hpp#L670-L701` | **Not driven**. Loops `step()` until `max_steps` (`#L682-L697`). Zero invocations of `bus_.tick()`. |
| **OOOCore::tick()** | `include/tinyarmsim/uarch/ooo_core.hpp#L173-L218` | **Not driven**. Advances pipeline stages and calls `lsu_.tick_cycle(cycles_)` (`#L176`) and `fetch_unit_.tick()` (`#L213`). `bus_.tick()` is never called. |
| **LoadStoreUnit** | `include/tinyarmsim/uarch/lsu.hpp#L91-L97` | `lsu_.tick_cycle()` only clears port counters (`loads_this_cycle_`, `stores_this_cycle_`). It does not tick `bus_`. |
| **baremetal_firmware_test** | `tests/baremetal_firmware_test.cpp#L16-L82` | **Not driven**. Instantiates `TimerDevice` (`#L22, L37`), but never calls `bus.tick()` anywhere in the suite. |

---

## 3. Firmware Loop Fidelity & Failure Modes

1. **Timer Polling Loop Hangs (Critical Bug)**:
   - Firmware polling `TIMER->VALUE` (`include/tinyarmsim/mmio/timer.hpp#L24`) or `TIMER->RIS` (`#L26`) in a busy-wait loop will encounter an **infinite hang**.
   - Because neither `IsaInterpreter` nor `OOOCore` invokes `bus.tick()`, `value_` never decrements (`#L58`) and `raw_irq_` never asserts (`#L49`).
   - In `IsaInterpreter::run()`, this triggers `CpuFaultException: Simulation execution exceeded max step limit` (`include/tinyarmsim/interpreter.hpp#L700`). In `OOOCore`, it triggers pipeline starvation/timeout.
2. **Missing Prescalers & Clock Ratio Distortions**:
   - `TimerDevice` directly decrements `value_ -= cycles` (`include/tinyarmsim/mmio/timer.hpp#L58`) without a prescaler register (`PSC`) or clock divider.
   - If manual ticking were enabled at 1 CPU cycle per instruction, a 100-cycle timer expires in 100 interpreter steps or ~25 cycles on a 4-wide OoO core, breaking real-world millisecond/microsecond delay calibration.
3. **Unrealistic Zero-Cycle Polling Fall-Throughs**:
   - **UART TX**: `UartDevice::read(0x04)` always asserts `TX_READY` (`include/tinyarmsim/mmio/uart.hpp#L34`). Loops `while (!(UART->STATUS & TX_READY))` fall through in 0 wait states.
   - **SPI / I2C Busy**: `SpiDevice` and `I2CDevice` complete transactions instantaneously within the register write (`include/tinyarmsim/mmio/spi.hpp#L38, L57-L80`; `include/tinyarmsim/mmio/i2c.hpp#L42, L67-L86`). Polling `BSY` or `RXNE` exits on the 1st iteration (0 cycles), preventing realistic contention and transfer modeling.
