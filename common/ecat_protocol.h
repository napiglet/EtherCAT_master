#pragma once
#ifndef ECAT_PROTOCOL_H
#define ECAT_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_NET_MAGIC 0x54414345u
#define ECAT_NET_VERSION 1u
#define ECAT_NET_DEFAULT_PORT 15000
#define ECAT_NET_MAX_SLAVES 64
#define ECAT_NET_MAX_PDO_COPY 64
#define ECAT_NET_MAX_TEXT 256
#define ECAT_NET_MAX_NAME 64

typedef enum ECAT_NetMessageType
{
   ECAT_NET_MSG_HELLO = 1,
   ECAT_NET_MSG_HELLO_REPLY = 2,
   ECAT_NET_MSG_STATUS = 3,
   ECAT_NET_MSG_COMMAND = 4,
   ECAT_NET_MSG_SDO_READ = 5,
   ECAT_NET_MSG_SDO_READ_REPLY = 6,
   ECAT_NET_MSG_SDO_WRITE = 7,
   ECAT_NET_MSG_SDO_WRITE_REPLY = 8,
   ECAT_NET_MSG_ERROR = 100
} ECAT_NetMessageType;

typedef enum ECAT_NetCommandType
{
   ECAT_NET_CMD_NONE = 0,
   ECAT_NET_CMD_START = 1,
   ECAT_NET_CMD_STOP = 2,
   ECAT_NET_CMD_RESET_STATISTICS = 3,
   ECAT_NET_CMD_SERVO_ENABLE = 10,
   ECAT_NET_CMD_SERVO_DISABLE = 11,
   ECAT_NET_CMD_SERVO_FAULT_RESET = 12,
   ECAT_NET_CMD_SERVO_SET_MODE = 13,
   ECAT_NET_CMD_SERVO_MOVE_ABS = 20,
   ECAT_NET_CMD_SERVO_JOG = 21,
   ECAT_NET_CMD_SERVO_HOME = 22,
   ECAT_NET_CMD_SERVO_STOP = 23,
   ECAT_NET_CMD_SERVO_MOVE_REL = 24,
   ECAT_NET_CMD_LMS_MOVE_ABS = 40,
   ECAT_NET_CMD_LMS_MOVE_VEL = 41,
   ECAT_NET_CMD_LMS_STOP = 42
} ECAT_NetCommandType;

typedef struct ECAT_NetHeader
{
   uint32_t magic;
   uint16_t version;
   uint16_t type;
   uint32_t size;
   uint32_t sequence;
} ECAT_NetHeader;

typedef struct ECAT_NetHello
{
   char client_name[ECAT_NET_MAX_NAME];
   int32_t period_us;
   int32_t request_operational;
} ECAT_NetHello;

typedef struct ECAT_NetRuntimeStatus
{
   int32_t connected;
   int32_t operational;
   int32_t slave_count;
   int32_t expected_wkc;
   int32_t last_wkc;
   int32_t total_cycles;
   int32_t wkc_errors;
   int32_t state_errors;
   int32_t cycle_us;
   int32_t min_cycle_us;
   int32_t max_cycle_us;
   double avg_cycle_us;
   int32_t period_us;
   int64_t dc_time_ns;
   char state_text[ECAT_NET_MAX_TEXT];
   char last_error[ECAT_NET_MAX_TEXT];
   char crc_status[ECAT_NET_MAX_TEXT];
} ECAT_NetRuntimeStatus;

typedef struct ECAT_NetSlaveStatus
{
   int32_t index;
   char name[ECAT_NET_MAX_NAME];
   uint16_t state;
   uint16_t al_status;
   uint32_t vendor_id;
   uint32_t product_code;
   uint32_t revision;
   uint32_t serial;
   uint32_t output_bytes;
   uint32_t input_bytes;
   int32_t has_dc;
   int32_t is_lost;
   uint16_t statusword;
   uint16_t controlword;
   int8_t mode_display;
   int32_t actual_position;
   int32_t actual_velocity;
   int32_t output_size;
   int32_t input_size;
   uint8_t outputs[ECAT_NET_MAX_PDO_COPY];
   uint8_t inputs[ECAT_NET_MAX_PDO_COPY];
} ECAT_NetSlaveStatus;

typedef struct ECAT_NetStatus
{
   ECAT_NetRuntimeStatus runtime;
   ECAT_NetSlaveStatus slaves[ECAT_NET_MAX_SLAVES];
} ECAT_NetStatus;

typedef struct ECAT_NetCommand
{
   int32_t command;
   int32_t slave_index;
   int32_t target_position;
   int32_t target_velocity;
   uint32_t velocity;
   uint32_t acceleration;
   uint32_t deceleration;
   int32_t mode;
   int32_t homing_method;
   uint32_t search_speed;
   uint32_t latch_speed;
} ECAT_NetCommand;

typedef struct ECAT_NetSdoRequest
{
   int32_t slave_index;
   uint16_t index;
   uint8_t subindex;
   uint8_t reserved;
   int32_t size;
   uint8_t data[256];
} ECAT_NetSdoRequest;

typedef struct ECAT_NetSdoReply
{
   int32_t result;
   int32_t size;
   uint8_t data[256];
} ECAT_NetSdoReply;

typedef struct ECAT_NetError
{
   int32_t code;
   char message[ECAT_NET_MAX_TEXT];
} ECAT_NetError;

#ifdef __cplusplus
}
#endif

#endif
