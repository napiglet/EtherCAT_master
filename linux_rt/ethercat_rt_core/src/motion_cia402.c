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
