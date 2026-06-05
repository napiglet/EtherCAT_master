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

Cia402State cia402_state_from_status(uint16_t statusword);
const char *cia402_status_text(Cia402State state);
const char *cia402_mode_text(int8_t mode);
const char *cia402_sequence_text(Cia402Sequence sequence);
uint16_t cia402_sequence_controlword(Cia402Sequence sequence,
                                     Cia402State state,
                                     uint16_t manual_controlword,
                                     int *target_reached);

#ifdef __cplusplus
}
#endif

#endif
