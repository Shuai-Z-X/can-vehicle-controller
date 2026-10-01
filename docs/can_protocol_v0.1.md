# CAN Protocol v0.1

- Version: v0.1
- Status: Frozen
- Freeze date: 2026-10-01
- Owner: <fill in your name>
- Scope: Classic CAN 2.0B communication, UDS subset, DID and DTC definitions

This document is for humans. `protocols/can_matrix.dbc` is the machine-readable
source of truth for CAN messages and signals. If this document and the DBC
disagree, the DBC wins and this document must be corrected.

## 1. Physical Layer

| Item | Value |
| --- | --- |
| Bus | Classic CAN 2.0B |
| Identifier | 11-bit standard ID |
| Bitrate | 500 kbps |
| DLC | 8 bytes for all application messages |
| Byte order | Little-endian (Intel) |
| CAN-H / CAN-L termination | 120 ohm at both ends of the bus |
| Common ground | Required between all nodes |

Suggested bit timing for 500 kbps:

| MCU | APB1 clock | Prescaler | BS1 | BS2 | SJW | Sample point |
| --- | --- | --- | --- | --- | --- | --- |
| STM32F103 | 36 MHz | 9 | 6 | 1 | 1 | 87.5% |
| STM32F407 | 42 MHz | 7 | 9 | 2 | 1 | 83.3% |

Always verify the bit timing in STM32CubeMX before generating code.

## 2. Node Roles

| Node | Role |
| --- | --- |
| VCU | Controller node. Sends `Control_Cmd`, publishes `VCU_Status`, acts as UDS server on `0x7E0` / `0x7E8`. |
| ACTUATOR | Actuator node. Receives `Control_Cmd`, publishes `Actuator_Status`, acts as UDS server on `0x7E1` / `0x7E9`. |
| TESTER | Linux SocketCAN host. Sends UDS requests, receives status frames and UDS responses. |

## 3. CAN Message List

| CAN ID | Name | Sender | Receivers | Cycle | Timeout |
| --- | --- | --- | --- | --- | --- |
| `0x100` | `VCU_Status` | VCU | ACTUATOR, TESTER | 20 ms | 300 ms |
| `0x101` | `Actuator_Status` | ACTUATOR | VCU, TESTER | 50 ms | 500 ms |
| `0x200` | `Control_Cmd` | VCU | ACTUATOR, TESTER | 20 ms | 300 ms |

UDS physical addressing:

| Direction | CAN ID |
| --- | --- |
| Tester to VCU request | `0x7E0` |
| VCU to tester response | `0x7E8` |
| Tester to ACTUATOR request | `0x7E1` |
| ACTUATOR to tester response | `0x7E9` |

v0.1 supports ISO-TP single-frame transport only. A request or response may
carry at most 7 bytes of service data. Multi-frame transport is a v0.2 item.

## 4. VCU_Status (`0x100`)

Sender: VCU. Cycle: 20 ms. DLC: 8.

| Byte | Signal | Type | Factor / Offset | Unit |
| --- | --- | --- | --- | --- |
| 0 | `VehMode` | uint8 | 1 / 0 | - |
| 1-2 | `VehSpeed` | uint16 LE | 0.1 / 0 | km/h |
| 3-4 | `BatteryVoltage` | uint16 LE | 0.01 / 0 | V |
| 5 | Fault bits | uint8 bitfield | - | - |
| 6 | `VCUStatusCounter` | uint8 | 1 / 0 | - |
| 7 | `VCUStatusChecksum` | uint8 | 1 / 0 | - |

Fault bits in byte 5:

| Bit | Signal | Meaning |
| --- | --- | --- |
| 0 | `FaultCanTimeout` | A required CAN message was not received before its timeout |
| 1 | `FaultSensorRange` | A sensor value is outside its valid range |
| 2 | `FaultActuator` | The actuator reports a fault |
| 3 | `FaultOverVoltage` | Over-voltage detected |
| 4 | `FaultUnderVoltage` | Under-voltage detected |
| 5-7 | `FaultReserved` | Reserved, sender writes 0 |

## 5. Actuator_Status (`0x101`)

Sender: ACTUATOR. Cycle: 50 ms. DLC: 8.

| Byte | Signal | Type | Factor / Offset | Unit |
| --- | --- | --- | --- | --- |
| 0 | `ActuatorMode` | uint8 | 1 / 0 | - |
| 1-2 | `ActuatorOutput` | uint16 LE | 0.1 / 0 | % |
| 3-4 | `MotorCurrent` | uint16 LE | 0.01 / 0 | A |
| 5 | `Temperature` | uint8 | 1 / -40 | degC |
| 6 | `ActuatorStatusCounter` | uint8 | 1 / 0 | - |
| 7 | `ActuatorStatusChecksum` | uint8 | 1 / 0 | - |

## 6. Control_Cmd (`0x200`)

Sender: VCU. Cycle: 20 ms. DLC: 8.

| Byte | Signal | Type | Factor / Offset | Unit |
| --- | --- | --- | --- | --- |
| 0 | `CmdMode` | uint8 | 1 / 0 | - |
| 1-2 | `TargetOutput` | uint16 LE | 0.1 / 0 | % |
| 3-5 | `CmdReserved` | uint24 LE | 1 / 0 | - |
| 6 | `CmdCounter` | uint8 | 1 / 0 | - |
| 7 | `CmdChecksum` | uint8 | 1 / 0 | - |

The sender must write 0 to `CmdReserved`. The receiver ignores reserved bits
but may log them for diagnostics.

## 7. Mode Values

`VehMode`, `ActuatorMode`, and `CmdMode` share the same value table:

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | OFF | Output disabled |
| 1 | STANDBY | Ready, output disabled |
| 2 | RUN | Normal control enabled |
| 3 | FAULT | Fault state, output disabled |

## 8. Rolling Counter and CRC-8

Every application message contains an 8-bit rolling counter and an 8-bit CRC.

- The counter starts at 0 after reset and increments by one for every frame.
- The counter wraps from 255 to 0.
- Receiver rule: `diff = (new_counter - last_counter) & 0xFF`.
- `diff == 1`: normal.
- `diff` in 2..3: accept the frame, but increment a missed-frame counter.
- `diff == 0`: duplicate frame, ignore.
- Any other value: count a counter error and update the last counter.

CRC parameters:

| Parameter | Value |
| --- | --- |
| Algorithm | CRC-8 |
| Polynomial | `0x07` |
| Initial value | `0x00` |
| Reflect input | No |
| Reflect output | No |
| Final XOR | `0x00` |
| Covered bytes | Bytes 0-6; byte 7 is the checksum itself |

The same algorithm must be used by firmware, the Linux host, and test scripts.

## 9. Timeouts and Fault Handling

| Message | Expected cycle | Timeout | Reaction |
| --- | --- | --- | --- |
| `VCU_Status` | 20 ms | 300 ms | Set `FaultCanTimeout`, store DTC `0xC10000` |
| `Actuator_Status` | 50 ms | 500 ms | Set `FaultActuator`, store DTC `0xC10002` |
| `Control_Cmd` | 20 ms | 300 ms | Actuator enters FAULT or STANDBY and stops output |

After the message is received again and a recovery condition is met, the
fault flag may be cleared, but the confirmed DTC remains until service `0x14`
clears it.

## 10. UDS Subset

ISO-TP single-frame format:

| Byte | Meaning |
| --- | --- |
| 0 | PCI: `0x0L`, where `L` is the number of service data bytes (1-7) |
| 1-7 | Service data |

Supported services:

| SID | Service | Sub-function / data | Positive response |
| --- | --- | --- | --- |
| `0x10` | Diagnostic session control | `0x01` default, `0x03` extended | `0x50` |
| `0x3E` | Tester present | `0x00` or `0x80` | `0x7E` |
| `0x22` | Read data by identifier | 2-byte DID | `0x62` |
| `0x2E` | Write data by identifier | 2-byte DID + data | `0x6E` |
| `0x19` | Read DTC information | sub-function `0x02` + status mask | `0x59` |
| `0x14` | Clear diagnostic information | `FF FF FF` means all DTCs | `0x54` |

Negative response format: `7F <SID> <NRC>`.

| NRC | Meaning |
| --- | --- |
| `0x12` | Sub-function not supported |
| `0x13` | Incorrect message length or invalid format |
| `0x22` | Conditions not correct |
| `0x31` | Request out of range |
| `0x7E` | Sub-function not supported in active session |
| `0x7F` | Service not supported in active session |

The extended session S3 timer is 5 seconds. When it expires, the ECU returns
to the default session.

## 11. DID Summary

See `protocols/did_table.json` for the machine-readable table.

| DID | Name | Access | Size |
| --- | --- | --- | --- |
| `0xF186` | ActiveDiagnosticSession | Read | 1 byte |
| `0x1234` | FirmwareVersion | Read | 4 bytes |
| `0x1235` | RuntimeStatus | Read | 6 bytes |
| `0x1236` | DtcCount | Read | 1 byte |
| `0x1237` | CanStatistics | Read | 4 bytes |
| `0x1238` | TestCommand | Write | 1 byte |

## 12. DTC Summary

See `protocols/dtc_table.json` for the machine-readable table.

| DTC | Name | Trigger |
| --- | --- | --- |
| `0xC10000` | CanTimeout | Expected CAN message not received before timeout |
| `0xC10001` | SensorOutOfRange | Sensor value outside valid range |
| `0xC10002` | ActuatorFault | Actuator status reports a fault |

DTC status bit definitions follow the table in `protocols/dtc_table.json`.

## 13. Change Control

Any change to message IDs, signal names, byte offsets, scaling, units,
cycle times, timeout values, counter rules, or CRC parameters requires:

1. A new protocol version, for example v0.2.
2. An update to `protocols/can_matrix.dbc`.
3. An update to `protocols/test_vectors.json`.
4. An update to both firmware and Linux host code.
5. A successful run of `tools/check_can_db.py` and the project tests.

## 14. Change Log

### v0.1 - 2026-10-01

- Froze the initial CAN matrix: `VCU_Status`, `Actuator_Status`, `Control_Cmd`.
- Selected 11-bit standard IDs, 500 kbps, and little-endian byte order.
- Added an 8-bit rolling counter and CRC-8 to every application message.
- Defined the UDS subset, DID table, and DTC table.
- Added golden test vectors and a DBC validation script.
