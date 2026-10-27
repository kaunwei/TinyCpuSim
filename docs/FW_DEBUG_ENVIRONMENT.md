# Firmware Debug Environment Baseline

## Toolchain

Both requested GNU Arm Embedded tools are available in `/usr/bin`:

- `arm-none-eabi-gcc`: 13.2.1 (GCC 15:13.2.1-2, dated 20231009)
- `arm-none-eabi-objdump`: 2.42 (GNU objdump 2.42-1ubuntu1+23)

## Execution limits

Firmware execution that exhausts the existing `--max-steps` limit ends with an error; limit exhaustion is not a successful firmware run. Interactive firmware may loop forever, so automated runs need both a small explicit step budget and an independent host-side timeout. The existing step-limit test exercises this error case and applies a five-second subprocess timeout.

A progress watchdog and pause/step interfaces are not implemented yet.
