#ifndef CAN_PROTOCOL_H
#define CAN_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_ID_VCU_STATUS      0x100U
#define CAN_ID_ACTUATOR_STATUS 0x101U
#define CAN_ID_CONTROL_CMD     0x200U

typedef enum {
    CAN_MODE_OFF = 0,
    CAN_MODE_STANDBY = 1,
    CAN_MODE_RUN = 2,
    CAN_MODE_FAULT = 3
} can_mode_t;

typedef struct {
    uint8_t mode;
    uint16_t speed_kmh_x10;
    uint16_t battery_voltage_x100;
    uint8_t fault_flags;
    uint8_t counter;
    uint8_t checksum;
} vcu_status_t;

typedef struct {
    uint8_t mode;
    uint16_t output_pct_x10;
    uint16_t current_a_x100;
    int16_t temperature_c;
    uint8_t counter;
    uint8_t checksum;
} actuator_status_t;

typedef struct {
    uint8_t mode;
    uint16_t target_output_pct_x10;
    uint8_t counter;
    uint8_t checksum;
} control_cmd_t;

uint8_t can_crc8(const uint8_t *data, uint32_t length);

bool can_encode_vcu_status(const vcu_status_t *status, uint8_t data[8]);
bool can_decode_vcu_status(const uint8_t data[8], vcu_status_t *status);

bool can_encode_actuator_status(const actuator_status_t *status, uint8_t data[8]);
bool can_decode_actuator_status(const uint8_t data[8], actuator_status_t *status);

bool can_encode_control_cmd(const control_cmd_t *cmd, uint8_t data[8]);
bool can_decode_control_cmd(const uint8_t data[8], control_cmd_t *cmd);

#endif