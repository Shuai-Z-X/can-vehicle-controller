# CAN Self-Test for F103 and F407

This directory contains one CAN loopback self-test for each MCU:

- `f103/node_b_actuator_f103`
- `f407/node_a_vcu_f407`

The self-test runs as a FreeRTOS task. Every second it:

1. Sends the known Control_Cmd frame `0x200#022C010000000934`.
2. Reads the frame back from CAN_RX_FIFO0 in loopback mode.
3. Checks ID, DLC, and all 8 data bytes.
4. Prints `CAN loopback OK` or `CAN loopback FAIL` on USART1 at 115200 8N1.

## Files added

For each project:

```text
Core/Inc/app_can_selftest.h
Core/Src/app_can_selftest.c
```

The file `Core/Src/freertos.c` was updated only inside USER CODE sections to
create the `canSelfTestTask`.

## Keil MDK steps

1. Open the project file:
   - `f103/node_b_actuator_f103/MDK-ARM/node_b_actuator_f103.uvprojx`
   - `f407/node_a_vcu_f407/MDK-ARM/node_a_vcu_f407.uvprojx`
2. In the Project window, expand `Application/User/Core`.
3. Right-click the group and choose `Add Existing Files to Group`.
4. Select `Core/Src/app_can_selftest.c`.
5. Build the project.

`Core/Inc` is already in the include path, so `app_can_selftest.h` is found
automatically.

## CAN mode

The self-test requires CAN loopback mode.

- F103 project: `hcan.Init.Mode` is already `CAN_MODE_LOOPBACK`.
- F407 project: open `node_a_vcu_f407.ioc`, set CAN1 mode to `Loopback`, and
  regenerate. Alternatively, edit `can.c` and set:

```c
hcan1.Init.Mode = CAN_MODE_LOOPBACK;
```

For Week 2, change both nodes back to `CAN_MODE_NORMAL` before connecting the
real CAN bus.

## UART output

USART1 is 115200 8N1. Expected output:

```text
CAN self-test: init OK
CAN loopback OK
CAN loopback OK
...
```

If the mode is not Loopback:

```text
CAN self-test: set CAN_MODE_LOOPBACK
```

## Notes

- The task stack is 256 words (1024 bytes), which is enough for HAL CAN and
  UART calls.
- The F407 CAN timing in the current `.ioc` must be checked before connecting
  the real bus. The self-test only checks loopback behavior, not the final
  500 kbps bus timing with another node.
- Do not connect a CAN transceiver for loopback self-test. The test uses the
  MCU internal loopback path.
