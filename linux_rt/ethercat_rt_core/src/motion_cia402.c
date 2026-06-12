#include "motion_cia402.h"

#include <limits.h>
#include <string.h>

#define CIA402_DEFAULT_PROFILE_VELOCITY 1000.0
#define CIA402_DEFAULT_ACCELERATION 10000.0
#define CIA402_POSITION_EPSILON 0.5
#define CIA402_VELOCITY_EPSILON 0.5

static double abs_double(double value)
{
   return value < 0.0 ? -value : value;
}

static double max_double(double lhs, double rhs)
{
   return lhs > rhs ? lhs : rhs;
}

static double clamp_double(double value, double min_value, double max_value)
{
   if (value < min_value)
   {
      return min_value;
   }
   if (value > max_value)
   {
      return max_value;
   }
   return value;
}

static int normalize_profile_type(int profile_type)
{
   switch (profile_type)
   {
   case CIA402_PROFILE_LMS:
   case CIA402_PROFILE_TRAPEZOIDAL:
   case CIA402_PROFILE_SCURVE:
   case CIA402_PROFILE_JERK_RATIO:
      return profile_type;
   default:
      return CIA402_PROFILE_TRAPEZOIDAL;
   }
}

static double normalize_jerk_ratio(double jerk_ratio)
{
   return clamp_double(jerk_ratio, 0.0, 1.0);
}

static int32_t round_to_i32(double value)
{
   if (value > (double)INT32_MAX)
   {
      return INT32_MAX;
   }
   if (value < (double)INT32_MIN)
   {
      return INT32_MIN;
   }
   return (int32_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static double configured_speed(int32_t value)
{
   double speed = abs_double((double)value);
   return speed > 0.0 ? speed : CIA402_DEFAULT_PROFILE_VELOCITY;
}

static double configured_accel(uint32_t value, double reference_speed)
{
   if (value > 0U)
   {
      return (double)value;
   }
   return max_double(CIA402_DEFAULT_ACCELERATION, reference_speed * 10.0);
}

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

const char *cia402_profile_type_text(int profile_type)
{
   switch (normalize_profile_type(profile_type))
   {
   case CIA402_PROFILE_LMS:
      return "LMS";
   case CIA402_PROFILE_SCURVE:
      return "SCurve";
   case CIA402_PROFILE_JERK_RATIO:
      return "JerkRatio";
   case CIA402_PROFILE_TRAPEZOIDAL:
   default:
      return "Trapezoidal";
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
   command->acceleration = 0;
   command->deceleration = 0;
   command->mode = 0;
   command->pulse_cycles = 20;
   command->relative = 0;
   command->profile_type = CIA402_PROFILE_TRAPEZOIDAL;
   command->jerk_ratio = 0.75;
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
      if (command->mode == CIA402_MODE_CSP)
      {
         output->controlword = 0x000fU;
      }
      else
      {
         new_setpoint =
            cycle_after_sequence >= 0 &&
            cycle_after_sequence < command->pulse_cycles;
         output->controlword =
            (uint16_t)(0x000fU | (new_setpoint ? 0x0010U : 0));
      }
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

void cia402_profile_reset(Cia402MotionProfile *profile)
{
   if (profile == 0)
   {
      return;
   }
   memset(profile, 0, sizeof(*profile));
   profile->done = 1;
}

void cia402_profile_begin(Cia402MotionProfile *profile,
                          const Cia402MotionCommand *command,
                          int32_t actual_position,
                          int32_t actual_velocity)
{
   double reference_speed;

   if (profile == 0 || command == 0)
   {
      return;
   }

   cia402_profile_reset(profile);
   if (command->type == CIA402_MOTION_NONE)
   {
      return;
   }

   profile->type = command->type;
   profile->active = 1;
   profile->done = 0;
   profile->relative = command->relative;
   profile->cycles = 0;
   profile->position = (double)actual_position;
   profile->velocity = (double)actual_velocity;
   profile->output_position = actual_position;
   profile->output_velocity = actual_velocity;
   profile->acceleration_state = 0.0;
   profile->profile_type = normalize_profile_type(command->profile_type);
   profile->jerk_ratio = normalize_jerk_ratio(command->jerk_ratio);

   switch (command->type)
   {
   case CIA402_MOTION_PROFILE_POSITION:
      reference_speed = configured_speed(command->profile_velocity);
      profile->target_position =
         command->relative ? (double)actual_position +
                                (double)command->target_position
                           : (double)command->target_position;
      profile->max_velocity = reference_speed;
      profile->acceleration =
         configured_accel(command->acceleration, reference_speed);
      profile->deceleration =
         configured_accel(command->deceleration, reference_speed);
      if (abs_double(profile->target_position - profile->position) <=
             CIA402_POSITION_EPSILON &&
          abs_double(profile->velocity) <= CIA402_VELOCITY_EPSILON)
      {
         profile->position = profile->target_position;
         profile->velocity = 0.0;
         profile->done = 1;
      }
      break;

   case CIA402_MOTION_JOG_VELOCITY:
      reference_speed = configured_speed(command->target_velocity);
      profile->target_velocity = (double)command->target_velocity;
      profile->max_velocity = reference_speed;
      profile->acceleration =
         configured_accel(command->acceleration, reference_speed);
      profile->deceleration =
         configured_accel(command->deceleration, reference_speed);
      break;

   case CIA402_MOTION_SERVO_STOP:
      reference_speed = configured_speed(actual_velocity);
      profile->target_velocity = 0.0;
      profile->max_velocity = reference_speed;
      profile->acceleration =
         configured_accel(command->acceleration, reference_speed);
      profile->deceleration =
         configured_accel(command->deceleration, reference_speed);
      break;

   case CIA402_MOTION_HOME:
      reference_speed = configured_speed(command->target_velocity);
      profile->target_velocity = (double)command->target_velocity;
      profile->max_velocity = reference_speed;
      profile->acceleration =
         configured_accel(command->acceleration, reference_speed);
      profile->deceleration =
         configured_accel(command->deceleration, reference_speed);
      break;

   default:
      profile->active = 0;
      profile->done = 1;
      break;
   }
}

static double ramp_velocity(double current,
                            double target,
                            double acceleration,
                            double deceleration,
                            double dt_s)
{
   double delta = target - current;
   double limit;

   if (abs_double(delta) <= CIA402_VELOCITY_EPSILON)
   {
      return target;
   }

   limit = ((target == 0.0 || abs_double(target) < abs_double(current))
               ? deceleration
               : acceleration) *
           dt_s;
   if (limit <= 0.0)
   {
      return target;
   }
   if (abs_double(delta) <= limit)
   {
      return target;
   }
   return current + (delta > 0.0 ? limit : -limit);
}

static double profile_ramp_time_s(const Cia402MotionProfile *profile)
{
   double ratio;

   if (profile == 0)
   {
      return 0.0;
   }

   switch (normalize_profile_type(profile->profile_type))
   {
   case CIA402_PROFILE_LMS:
      return 0.35;
   case CIA402_PROFILE_SCURVE:
      return 0.25;
   case CIA402_PROFILE_JERK_RATIO:
      ratio = normalize_jerk_ratio(profile->jerk_ratio);
      return 0.03 + ratio * 0.30;
   case CIA402_PROFILE_TRAPEZOIDAL:
   default:
      return 0.0;
   }
}

static double slew_acceleration(Cia402MotionProfile *profile,
                                double target_acceleration,
                                double dt_s)
{
   double ramp_time;
   double jerk_limit;
   double max_acceleration;
   double max_delta;
   double delta;

   ramp_time = profile_ramp_time_s(profile);
   if (ramp_time <= 0.0)
   {
      profile->acceleration_state = target_acceleration;
      return target_acceleration;
   }

   max_acceleration =
      max_double(profile->acceleration, profile->deceleration);
   if (max_acceleration <= 0.0)
   {
      profile->acceleration_state = target_acceleration;
      return target_acceleration;
   }

   jerk_limit = max_acceleration / ramp_time;
   max_delta = jerk_limit * dt_s;
   delta = target_acceleration - profile->acceleration_state;
   if (abs_double(delta) <= max_delta)
   {
      profile->acceleration_state = target_acceleration;
   }
   else
   {
      profile->acceleration_state += delta > 0.0 ? max_delta : -max_delta;
   }
   return profile->acceleration_state;
}

static double ramp_velocity_by_profile(Cia402MotionProfile *profile,
                                       double target,
                                       double dt_s)
{
   double delta;
   double target_acceleration;
   double acceleration;
   double next_velocity;
   double limit_acceleration;

   if (profile_ramp_time_s(profile) <= 0.0)
   {
      return ramp_velocity(profile->velocity, target,
                           profile->acceleration,
                           profile->deceleration,
                           dt_s);
   }

   delta = target - profile->velocity;
   if (abs_double(delta) <= CIA402_VELOCITY_EPSILON)
   {
      profile->acceleration_state = 0.0;
      return target;
   }

   limit_acceleration =
      (target == 0.0 || abs_double(target) < abs_double(profile->velocity))
         ? profile->deceleration
         : profile->acceleration;
   target_acceleration = delta > 0.0 ? limit_acceleration
                                     : -limit_acceleration;
   acceleration = slew_acceleration(profile, target_acceleration, dt_s);
   next_velocity = profile->velocity + acceleration * dt_s;
   if ((delta > 0.0 && next_velocity > target) ||
       (delta < 0.0 && next_velocity < target))
   {
      next_velocity = target;
      profile->acceleration_state = 0.0;
   }
   return next_velocity;
}

static void step_position_profile_trapezoidal(Cia402MotionProfile *profile,
                                              double dt_s)
{
   double error;
   double direction;
   double velocity_along;
   double stop_distance;
   double remaining;

   if (profile->done)
   {
      return;
   }

   error = profile->target_position - profile->position;
   remaining = abs_double(error);
   if (remaining <= CIA402_POSITION_EPSILON &&
       abs_double(profile->velocity) <= CIA402_VELOCITY_EPSILON)
   {
      profile->position = profile->target_position;
      profile->velocity = 0.0;
      profile->done = 1;
      return;
   }

   direction = error >= 0.0 ? 1.0 : -1.0;
   velocity_along = profile->velocity * direction;
   if (velocity_along < 0.0)
   {
      velocity_along += profile->deceleration * dt_s;
      if (velocity_along > 0.0)
      {
         velocity_along = 0.0;
      }
   }
   else
   {
      stop_distance = (velocity_along * velocity_along) /
                      (2.0 * profile->deceleration);
      if (remaining <= stop_distance + CIA402_POSITION_EPSILON)
      {
         velocity_along -= profile->deceleration * dt_s;
         if (velocity_along < 0.0)
         {
            velocity_along = 0.0;
         }
      }
      else
      {
         velocity_along += profile->acceleration * dt_s;
         if (velocity_along > profile->max_velocity)
         {
            velocity_along = profile->max_velocity;
         }
      }
   }

   profile->velocity = direction * velocity_along;
   profile->position += profile->velocity * dt_s;

   if ((direction > 0.0 && profile->position >= profile->target_position) ||
       (direction < 0.0 && profile->position <= profile->target_position))
   {
      profile->position = profile->target_position;
      profile->velocity = 0.0;
      profile->done = 1;
   }
}

static void step_position_profile_smooth(Cia402MotionProfile *profile,
                                         double dt_s)
{
   double error;
   double direction;
   double velocity_along;
   double next_velocity_along;
   double stop_distance;
   double remaining;
   double target_velocity_along;
   double target_acceleration;
   double acceleration;

   if (profile->done)
   {
      return;
   }

   error = profile->target_position - profile->position;
   remaining = abs_double(error);
   if (remaining <= CIA402_POSITION_EPSILON &&
       abs_double(profile->velocity) <= CIA402_VELOCITY_EPSILON)
   {
      profile->position = profile->target_position;
      profile->velocity = 0.0;
      profile->acceleration_state = 0.0;
      profile->done = 1;
      return;
   }

   direction = error >= 0.0 ? 1.0 : -1.0;
   velocity_along = profile->velocity * direction;
   if (velocity_along < 0.0)
   {
      target_velocity_along = 0.0;
   }
   else if (velocity_along <= CIA402_VELOCITY_EPSILON)
   {
      target_velocity_along = profile->max_velocity;
   }
   else
   {
      stop_distance = (velocity_along * velocity_along) /
                      (2.0 * profile->deceleration);
      target_velocity_along =
         remaining <= stop_distance + CIA402_POSITION_EPSILON
            ? 0.0
            : profile->max_velocity;
   }

   if (target_velocity_along > velocity_along + CIA402_VELOCITY_EPSILON)
   {
      target_acceleration = profile->acceleration;
   }
   else if (target_velocity_along < velocity_along - CIA402_VELOCITY_EPSILON)
   {
      target_acceleration = -profile->deceleration;
   }
   else
   {
      target_acceleration = 0.0;
   }

   acceleration = slew_acceleration(profile, target_acceleration, dt_s);
   next_velocity_along = velocity_along + acceleration * dt_s;
   if (target_acceleration > 0.0 &&
       next_velocity_along > target_velocity_along)
   {
      next_velocity_along = target_velocity_along;
   }
   else if (target_acceleration < 0.0 &&
            next_velocity_along < target_velocity_along)
   {
      next_velocity_along = target_velocity_along;
   }

   if (next_velocity_along < 0.0)
   {
      next_velocity_along = 0.0;
   }
   if (next_velocity_along > profile->max_velocity)
   {
      next_velocity_along = profile->max_velocity;
   }

   profile->velocity = direction * next_velocity_along;
   profile->position += profile->velocity * dt_s;

   if ((direction > 0.0 && profile->position >= profile->target_position) ||
       (direction < 0.0 && profile->position <= profile->target_position))
   {
      profile->position = profile->target_position;
      profile->velocity = 0.0;
      profile->acceleration_state = 0.0;
      profile->done = 1;
   }
}

static void step_position_profile(Cia402MotionProfile *profile, double dt_s)
{
   if (profile_ramp_time_s(profile) <= 0.0)
   {
      step_position_profile_trapezoidal(profile, dt_s);
   }
   else
   {
      step_position_profile_smooth(profile, dt_s);
   }
}

int cia402_profile_step(Cia402MotionProfile *profile,
                        const Cia402MotionCommand *command,
                        int32_t actual_position,
                        int32_t actual_velocity,
                        int period_us,
                        Cia402PdoOutput *output)
{
   double dt_s;

   if (profile == 0 || command == 0 || output == 0 ||
       command->type == CIA402_MOTION_NONE)
   {
      return 1;
   }

   if (!profile->active || profile->type != command->type)
   {
      cia402_profile_begin(profile, command, actual_position, actual_velocity);
   }

   dt_s = (period_us > 0 ? (double)period_us : 1000.0) / 1000000.0;
   if (dt_s <= 0.0)
   {
      dt_s = 0.001;
   }

   switch (command->type)
   {
   case CIA402_MOTION_PROFILE_POSITION:
      step_position_profile(profile, dt_s);
      output->controlword = 0x000fU;
      output->target_position = round_to_i32(profile->position);
      output->target_velocity = round_to_i32(profile->velocity);
      output->mode = command->mode != 0 ? command->mode : CIA402_MODE_CSP;
      break;

   case CIA402_MOTION_JOG_VELOCITY:
      profile->velocity =
         ramp_velocity_by_profile(profile, profile->target_velocity, dt_s);
      profile->position = (double)actual_position;
      profile->done = profile->target_velocity == 0.0 &&
                      abs_double(profile->velocity) <=
                         CIA402_VELOCITY_EPSILON;
      output->controlword = 0x000fU;
      output->target_position = actual_position;
      output->target_velocity = round_to_i32(profile->velocity);
      output->mode = command->mode != 0 ? command->mode : CIA402_MODE_CSV;
      break;

   case CIA402_MOTION_SERVO_STOP:
      profile->velocity =
         ramp_velocity_by_profile(profile, 0.0, dt_s);
      profile->position = (double)actual_position;
      profile->done = abs_double(profile->velocity) <= CIA402_VELOCITY_EPSILON;
      if (profile->done)
      {
         profile->velocity = 0.0;
      }
      output->controlword = 0x010fU;
      output->target_position = actual_position;
      output->target_velocity = round_to_i32(profile->velocity);
      output->mode = command->mode != 0 ? command->mode : CIA402_MODE_CSV;
      break;

   case CIA402_MOTION_HOME:
      profile->position = (double)actual_position;
      profile->velocity =
         ramp_velocity_by_profile(profile, profile->target_velocity, dt_s);
      output->controlword = 0x001fU;
      output->target_position = actual_position;
      output->target_velocity = round_to_i32(profile->velocity);
      output->mode = CIA402_MODE_HOMING;
      profile->done = 0;
      break;

   default:
      profile->done = 1;
      break;
   }

   profile->output_position = output->target_position;
   profile->output_velocity = output->target_velocity;
   ++profile->cycles;
   return profile->done;
}
