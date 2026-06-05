#include "motion_cia402.h"

Cia402State cia402_state_from_status(uint16_t statusword)
{
   if ((statusword & 0x004fU) == 0x0000U)
   {
      return CIA402_STATE_NOT_READY;
   }
   if ((statusword & 0x004fU) == 0x0040U)
   {
      return CIA402_STATE_SWITCH_ON_DISABLED;
   }
   if ((statusword & 0x006fU) == 0x0021U)
   {
      return CIA402_STATE_READY_TO_SWITCH_ON;
   }
   if ((statusword & 0x006fU) == 0x0023U)
   {
      return CIA402_STATE_SWITCHED_ON;
   }
   if ((statusword & 0x006fU) == 0x0027U)
   {
      return CIA402_STATE_OPERATION_ENABLED;
   }
   if ((statusword & 0x006fU) == 0x0007U)
   {
      return CIA402_STATE_QUICK_STOP_ACTIVE;
   }
   if ((statusword & 0x004fU) == 0x000fU)
   {
      return CIA402_STATE_FAULT_REACTION_ACTIVE;
   }
   if ((statusword & 0x004fU) == 0x0008U)
   {
      return CIA402_STATE_FAULT;
   }
   return CIA402_STATE_UNKNOWN;
}

const char *cia402_status_text(Cia402State state)
{
   switch (state)
   {
   case CIA402_STATE_NOT_READY:
      return "NotReady";
   case CIA402_STATE_SWITCH_ON_DISABLED:
      return "SwitchOnDisabled";
   case CIA402_STATE_READY_TO_SWITCH_ON:
      return "ReadyToSwitchOn";
   case CIA402_STATE_SWITCHED_ON:
      return "SwitchedOn";
   case CIA402_STATE_OPERATION_ENABLED:
      return "OperationEnabled";
   case CIA402_STATE_QUICK_STOP_ACTIVE:
      return "QuickStopActive";
   case CIA402_STATE_FAULT_REACTION_ACTIVE:
      return "FaultReactionActive";
   case CIA402_STATE_FAULT:
      return "Fault";
   default:
      return "Unknown";
   }
}

const char *cia402_mode_text(int8_t mode)
{
   switch (mode)
   {
   case 1:
      return "PP";
   case 3:
      return "PV";
   case 6:
      return "HM";
   case 8:
      return "CSP";
   case 9:
      return "CSV";
   case 10:
      return "CST";
   default:
      return "-";
   }
}

const char *cia402_sequence_text(Cia402Sequence sequence)
{
   switch (sequence)
   {
   case CIA402_SEQ_FAULT_RESET:
      return "FaultReset";
   case CIA402_SEQ_SHUTDOWN:
      return "Shutdown";
   case CIA402_SEQ_SWITCH_ON:
      return "SwitchOn";
   case CIA402_SEQ_ENABLE:
      return "EnableOperation";
   case CIA402_SEQ_DISABLE:
      return "DisableVoltage";
   default:
      return "None";
   }
}

const char *cia402_motion_text(Cia402MotionType type)
{
   switch (type)
   {
   case CIA402_MOTION_SERVO_STOP:
      return "ServoStop";
   case CIA402_MOTION_JOG_VELOCITY:
      return "JogVelocity";
   case CIA402_MOTION_PROFILE_POSITION:
      return "ProfilePosition";
   case CIA402_MOTION_HOME:
      return "Home";
   default:
      return "None";
   }
}

uint16_t cia402_sequence_controlword(Cia402Sequence sequence,
                                     Cia402State state,
                                     uint16_t manual_controlword,
                                     int *target_reached)
{
   if (target_reached != 0)
   {
      *target_reached = 0;
   }

   switch (sequence)
   {
   case CIA402_SEQ_NONE:
      if (target_reached != 0)
      {
         *target_reached = 1;
      }
      return manual_controlword;

   case CIA402_SEQ_FAULT_RESET:
      if (state == CIA402_STATE_FAULT ||
          state == CIA402_STATE_FAULT_REACTION_ACTIVE)
      {
         return 0x0080U;
      }
      if (target_reached != 0)
      {
         *target_reached = 1;
      }
      return 0x0000U;

   case CIA402_SEQ_SHUTDOWN:
      if (state == CIA402_STATE_READY_TO_SWITCH_ON)
      {
         if (target_reached != 0)
         {
            *target_reached = 1;
         }
      }
      return 0x0006U;

   case CIA402_SEQ_SWITCH_ON:
      if (state == CIA402_STATE_SWITCHED_ON ||
          state == CIA402_STATE_OPERATION_ENABLED)
      {
         if (target_reached != 0)
         {
            *target_reached = 1;
         }
         return 0x0007U;
      }
      if (state == CIA402_STATE_READY_TO_SWITCH_ON)
      {
         return 0x0007U;
      }
      if (state == CIA402_STATE_FAULT ||
          state == CIA402_STATE_FAULT_REACTION_ACTIVE)
      {
         return 0x0080U;
      }
      return 0x0006U;

   case CIA402_SEQ_ENABLE:
      if (state == CIA402_STATE_OPERATION_ENABLED)
      {
         if (target_reached != 0)
         {
            *target_reached = 1;
         }
         return 0x000fU;
      }
      if (state == CIA402_STATE_SWITCHED_ON)
      {
         return 0x000fU;
      }
      if (state == CIA402_STATE_READY_TO_SWITCH_ON)
      {
         return 0x0007U;
      }
      if (state == CIA402_STATE_FAULT ||
          state == CIA402_STATE_FAULT_REACTION_ACTIVE)
      {
         return 0x0080U;
      }
      return 0x0006U;

   case CIA402_SEQ_DISABLE:
      if (state == CIA402_STATE_SWITCH_ON_DISABLED)
      {
         if (target_reached != 0)
         {
            *target_reached = 1;
         }
      }
      return 0x0000U;

   default:
      return manual_controlword;
   }
}

void cia402_motion_init(Cia402MotionCommand *command)
{
   if (command == 0)
   {
      return;
   }

   command->type = CIA402_MOTION_NONE;
   command->target_position = 0;
   command->target_velocity = 0;
   command->profile_velocity = 0;
   command->mode = 0;
   command->pulse_cycles = 20;
}

void cia402_motion_apply(const Cia402MotionCommand *command,
                         int sequence_done,
                         int cycle_after_sequence,
                         Cia402PdoOutput *output)
{
   int new_setpoint;

   if (command == 0 || output == 0 || command->type == CIA402_MOTION_NONE)
   {
      return;
   }

   if (command->type == CIA402_MOTION_SERVO_STOP)
   {
      output->controlword = 0x010fU;
      output->target_velocity = 0;
      if (output->mode == 0)
      {
         output->mode = CIA402_MODE_CSV;
      }
      return;
   }

   if (!sequence_done)
   {
      return;
   }

   switch (command->type)
   {
   case CIA402_MOTION_JOG_VELOCITY:
      output->controlword = 0x000fU;
      output->target_velocity = command->target_velocity;
      output->mode = command->mode != 0 ? command->mode : CIA402_MODE_CSV;
      break;

   case CIA402_MOTION_PROFILE_POSITION:
      new_setpoint =
         cycle_after_sequence >= 0 &&
         cycle_after_sequence < command->pulse_cycles;
      output->controlword = (uint16_t)(0x000fU | (new_setpoint ? 0x0010U : 0));
      output->target_position = command->target_position;
      output->target_velocity = command->profile_velocity;
      output->mode = command->mode != 0 ? command->mode : CIA402_MODE_PROFILE_POSITION;
      break;

   case CIA402_MOTION_HOME:
      output->controlword = 0x001fU;
      output->target_velocity = command->target_velocity;
      output->mode = CIA402_MODE_HOMING;
      break;

   default:
      break;
   }
}
