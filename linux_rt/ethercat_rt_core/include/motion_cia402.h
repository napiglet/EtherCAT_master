#pragma once
#ifndef ECAT_MOTION_CIA402_H
#define ECAT_MOTION_CIA402_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Cia402State
{
   CIA402_STATE_NOT_READY = 0,
   CIA402_STATE_SWITCH_ON_DISABLED,
   CIA402_STATE_READY_TO_SWITCH_ON,
   CIA402_STATE_SWITCHED_ON,
   CIA402_STATE_OPERATION_ENABLED,
   CIA402_STATE_QUICK_STOP_ACTIVE,
   CIA402_STATE_FAULT_REACTION_ACTIVE,
   CIA402_STATE_FAULT,
   CIA402_STATE_UNKNOWN
} Cia402State;

typedef enum Cia402Sequence
{
   CIA402_SEQ_NONE = 0,
   CIA402_SEQ_FAULT_RESET,
   CIA402_SEQ_SHUTDOWN,
   CIA402_SEQ_SWITCH_ON,
   CIA402_SEQ_ENABLE,
   CIA402_SEQ_DISABLE
} Cia402Sequence;

typedef enum Cia402Mode
{
   CIA402_MODE_PROFILE_POSITION = 1,
   CIA402_MODE_PROFILE_VELOCITY = 3,
   CIA402_MODE_HOMING = 6,
   CIA402_MODE_CSP = 8,
   CIA402_MODE_CSV = 9,
   CIA402_MODE_CST = 10
} Cia402Mode;

typedef enum Cia402MotionType
{
   CIA402_MOTION_NONE = 0,
   CIA402_MOTION_SERVO_STOP,
   CIA402_MOTION_JOG_VELOCITY,
   CIA402_MOTION_PROFILE_POSITION,
   CIA402_MOTION_HOME
} Cia402MotionType;

typedef struct Cia402MotionCommand
{
   Cia402MotionType type;
   int32_t target_position;
   int32_t target_velocity;
   int32_t profile_velocity;
   uint32_t acceleration;
   uint32_t deceleration;
   int8_t mode;
   int pulse_cycles;
   int relative;
} Cia402MotionCommand;

typedef struct Cia402PdoOutput
{
   uint16_t controlword;
   int32_t target_position;
   int32_t target_velocity;
   int8_t mode;
} Cia402PdoOutput;

typedef struct Cia402MotionProfile
{
   Cia402MotionType type;
   int active;
   int done;
   int relative;
   int64_t cycles;
   double position;
   double velocity;
   double target_position;
   double target_velocity;
   double max_velocity;
   double acceleration;
   double deceleration;
   int32_t output_position;
   int32_t output_velocity;
} Cia402MotionProfile;

Cia402State cia402_state_from_status(uint16_t statusword);
const char *cia402_status_text(Cia402State state);
const char *cia402_mode_text(int8_t mode);
const char *cia402_sequence_text(Cia402Sequence sequence);
const char *cia402_motion_text(Cia402MotionType type);
uint16_t cia402_sequence_controlword(Cia402Sequence sequence,
                                     Cia402State state,
                                     uint16_t manual_controlword,
                                     int *target_reached);
void cia402_motion_init(Cia402MotionCommand *command);
void cia402_motion_apply(const Cia402MotionCommand *command,
                         int sequence_done,
                         int cycle_after_sequence,
                         Cia402PdoOutput *output);
void cia402_profile_reset(Cia402MotionProfile *profile);
void cia402_profile_begin(Cia402MotionProfile *profile,
                          const Cia402MotionCommand *command,
                          int32_t actual_position,
                          int32_t actual_velocity);
int cia402_profile_step(Cia402MotionProfile *profile,
                        const Cia402MotionCommand *command,
                        int32_t actual_position,
                        int32_t actual_velocity,
                        int period_us,
                        Cia402PdoOutput *output);

#ifdef __cplusplus
}
#endif

#endif
