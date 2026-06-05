#pragma once
#ifndef ECAT_RT_IPC_H
#define ECAT_RT_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_RT_IPC_VERSION 1U
#define ECAT_RT_MAX_SLAVES 200
#define ECAT_RT_MAX_PDO_BYTES 256
#define ECAT_RT_MAX_MESSAGE 256

typedef enum ECAT_RtCommandType
{
   ECAT_RT_CMD_NONE = 0,
   ECAT_RT_CMD_START = 1,
   ECAT_RT_CMD_STOP = 2,
   ECAT_RT_CMD_SERVO_ENABLE = 10,
   ECAT_RT_CMD_SERVO_DISABLE = 11,
   ECAT_RT_CMD_SERVO_RESET = 12,
   ECAT_RT_CMD_MOVE_ABS = 20,
   ECAT_RT_CMD_MOVE_VEL = 21,
   ECAT_RT_CMD_HOME = 22,
   ECAT_RT_CMD_HALT = 23,
   ECAT_RT_CMD_LMS_MOVE_ABS = 40,
   ECAT_RT_CMD_LMS_MOVE_VEL = 41,
   ECAT_RT_CMD_LMS_STOP = 42
} ECAT_RtCommandType;

typedef struct ECAT_RtCommand
{
   unsigned int sequence;
   unsigned int type;
   int slave_index;
   int target_position;
   int target_velocity;
   unsigned int velocity;
   unsigned int acceleration;
   unsigned int deceleration;
   int mode;
   int reserved[8];
} ECAT_RtCommand;

typedef struct ECAT_RtSlaveStatus
{
   int slave_index;
   unsigned short state;
   unsigned short al_status;
   unsigned short statusword;
   unsigned short controlword;
   signed char mode_display;
   int actual_position;
   int actual_velocity;
   int target_position;
   int target_velocity;
   int fault;
   int warning;
   int target_reached;
   unsigned int input_size;
   unsigned int output_size;
   unsigned char inputs[ECAT_RT_MAX_PDO_BYTES];
   unsigned char outputs[ECAT_RT_MAX_PDO_BYTES];
} ECAT_RtSlaveStatus;

typedef struct ECAT_RtStatus
{
   unsigned int version;
   unsigned int status_sequence;
   unsigned int last_command_sequence;
   int running;
   int connected;
   int operational;
   int slave_count;
   int period_us;
   int cycle_us;
   int max_cycle_us;
   int expected_wkc;
   int last_wkc;
   int wkc_errors;
   long long dc_time_ns;
   char state_text[ECAT_RT_MAX_MESSAGE];
   char last_error[ECAT_RT_MAX_MESSAGE];
   ECAT_RtSlaveStatus slaves[ECAT_RT_MAX_SLAVES];
} ECAT_RtStatus;

typedef struct ECAT_RtSharedMemory
{
   unsigned int version;
   volatile long command_lock;
   volatile long status_lock;
   ECAT_RtCommand command;
   ECAT_RtStatus status;
} ECAT_RtSharedMemory;

#ifdef __cplusplus
}
#endif

#endif
