#include "can_protocol.h"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

uint8_t can_crc8(const uint8_t *data, uint32_t length)
{
    uint8_t crc = 0U;
    uint32_t i;
    int bit;

    for (i = 0U; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x80U) {
                crc = (uint8_t)((crc << 1) ^ 0x07U);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

bool can_encode_vcu_status(const vcu_status_t *status, uint8_t data[8])
{
    if (status == 0) {
        return false;
    }

    data[0] = status->mode;
    write_le16(&data[1], status->speed_kmh_x10);
    write_le16(&data[3], status->battery_voltage_x100);
    data[5] = status->fault_flags;
    data[6] = status->counter;
    data[7] = can_crc8(data, 7U);
    return true;
}

bool can_decode_vcu_status(const uint8_t data[8], vcu_status_t *status)
{
    if (status == 0 || data[7] != can_crc8(data, 7U)) {
        return false;
    }

    status->mode = data[0];
    status->speed_kmh_x10 = read_le16(&data[1]);
    status->battery_voltage_x100 = read_le16(&data[3]);
    status->fault_flags = data[5];
    status->counter = data[6];
    status->checksum = data[7];
    return true;
}

bool can_encode_actuator_status(const actuator_status_t *status, uint8_t data[8])
{
    int16_t raw_temperature;

    if (status == 0) {
        return false;
    }

    raw_temperature = (int16_t)(status->temperature_c + 40);
    if (raw_temperature < 0) {
        raw_temperature = 0;
    } else if (raw_temperature > 255) {
        raw_temperature = 255;
    }

    data[0] = status->mode;
    write_le16(&data[1], status->output_pct_x10);
    write_le16(&data[3], status->current_a_x100);
    data[5] = (uint8_t)raw_temperature;
    data[6] = status->counter;
    data[7] = can_crc8(data, 7U);
    return true;
}

bool can_decode_actuator_status(const uint8_t data[8], actuator_status_t *status)
{
    if (status == 0 || data[7] != can_crc8(data, 7U)) {
        return false;
    }

    status->mode = data[0];
    status->output_pct_x10 = read_le16(&data[1]);
    status->current_a_x100 = read_le16(&data[3]);
    status->temperature_c = (int16_t)data[5] - 40;
    status->counter = data[6];
    status->checksum = data[7];
    return true;
}

bool can_encode_control_cmd(const control_cmd_t *cmd, uint8_t data[8])
{
    if (cmd == 0) {
        return false;
    }

    data[0] = cmd->mode;
    write_le16(&data[1], cmd->target_output_pct_x10);
    data[3] = 0U;
    data[4] = 0U;
    data[5] = 0U;
    data[6] = cmd->counter;
    data[7] = can_crc8(data, 7U);
    return true;
}

bool can_decode_control_cmd(const uint8_t data[8], control_cmd_t *cmd)
{
    if (cmd == 0 || data[7] != can_crc8(data, 7U)) {
        return false;
    }

    cmd->mode = data[0];
    cmd->target_output_pct_x10 = read_le16(&data[1]);
    cmd->counter = data[6];
    cmd->checksum = data[7];
    return true;
}