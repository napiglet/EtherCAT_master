#include "motion_cia402.h"

#include <stdio.h>
#include <stdlib.h>

static void check_int(const char *name, int actual, int expected)
{
   if (actual != expected)
   {
      fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
      exit(1);
   }
}

static void check_true(const char *name, int condition)
{
   if (!condition)
   {
      fprintf(stderr, "%s failed\n", name);
      exit(1);
   }
}

static Cia402MotionCommand make_position_command(int target, int relative)
{
   Cia402MotionCommand command;

   cia402_motion_init(&command);
   command.type = CIA402_MOTION_PROFILE_POSITION;
   command.mode = CIA402_MODE_CSP;
   command.target_position = target;
   command.profile_velocity = 1000;
   command.acceleration = 5000;
   command.deceleration = 5000;
   command.relative = relative;
   return command;
}

static void test_absolute_profile(void)
{
   Cia402MotionCommand command = make_position_command(1000, 0);
   Cia402MotionProfile profile;
   Cia402PdoOutput output = {0};
   int done = 0;
   int i;

   cia402_profile_reset(&profile);
   for (i = 0; i < 5000 && !done; ++i)
   {
      done = cia402_profile_step(&profile, &command,
                                 output.target_position, 0, 1000, &output);
      check_true("absolute profile mode", output.mode == CIA402_MODE_CSP);
      check_true("absolute profile controlword", output.controlword == 0x000fU);
      check_true("absolute profile lower bound", output.target_position >= 0);
      check_true("absolute profile upper bound", output.target_position <= 1000);
   }

   check_true("absolute profile completed", done);
   check_int("absolute profile target", output.target_position, 1000);
   check_int("absolute profile velocity", output.target_velocity, 0);
}

static void test_profile_types(void)
{
   int profile_types[] = {
      CIA402_PROFILE_LMS,
      CIA402_PROFILE_TRAPEZOIDAL,
      CIA402_PROFILE_SCURVE,
      CIA402_PROFILE_JERK_RATIO
   };
   int i;

   for (i = 0; i < (int)(sizeof(profile_types) / sizeof(profile_types[0])); ++i)
   {
      Cia402MotionCommand command = make_position_command(1000, 0);
      Cia402MotionProfile profile;
      Cia402PdoOutput output = {0};
      int done = 0;
      int cycle;

      command.profile_type = profile_types[i];
      command.jerk_ratio = 0.75;
      if (command.profile_type == CIA402_PROFILE_LMS)
      {
         command.jerk_ratio = 0.35;
      }
      cia402_profile_reset(&profile);
      for (cycle = 0; cycle < 10000 && !done; ++cycle)
      {
         done = cia402_profile_step(&profile, &command,
                                    output.target_position, 0, 1000, &output);
         check_true("profile type lower bound", output.target_position >= 0);
         check_true("profile type upper bound", output.target_position <= 1000);
      }

      if (!done)
      {
         fprintf(stderr, "profile type %d completed failed, pos=%d vel=%d\n",
                 command.profile_type, output.target_position,
                 output.target_velocity);
         exit(1);
      }
      check_int("profile type target", output.target_position, 1000);
      check_int("profile type velocity", output.target_velocity, 0);
   }
}

static void test_relative_profile(void)
{
   Cia402MotionCommand command = make_position_command(-50, 1);
   Cia402MotionProfile profile;
   Cia402PdoOutput output = {0};
   int done = 0;
   int i;

   cia402_profile_reset(&profile);
   for (i = 0; i < 2000 && !done; ++i)
   {
      done = cia402_profile_step(&profile, &command, 200, 0, 1000, &output);
   }

   check_true("relative profile completed", done);
   check_int("relative profile target", output.target_position, 150);
}

static void test_jog_velocity_profile(void)
{
   Cia402MotionCommand command;
   Cia402MotionProfile profile;
   Cia402PdoOutput output = {0};
   int done = 1;
   int i;

   cia402_motion_init(&command);
   command.type = CIA402_MOTION_JOG_VELOCITY;
   command.mode = CIA402_MODE_CSV;
   command.target_velocity = 100;
   command.acceleration = 1000;
   command.deceleration = 1000;

   cia402_profile_reset(&profile);
   for (i = 0; i < 200; ++i)
   {
      done = cia402_profile_step(&profile, &command, 0,
                                 output.target_velocity, 1000, &output);
   }

   check_true("jog remains active", !done);
   check_int("jog mode", output.mode, CIA402_MODE_CSV);
   check_int("jog velocity", output.target_velocity, 100);
   check_int("jog controlword", output.controlword, 0x000fU);
}

static void test_stop_profile(void)
{
   Cia402MotionCommand command;
   Cia402MotionProfile profile;
   Cia402PdoOutput output = {0};
   int done = 0;
   int i;

   cia402_motion_init(&command);
   command.type = CIA402_MOTION_SERVO_STOP;
   command.mode = CIA402_MODE_CSV;
   command.deceleration = 1000;

   cia402_profile_reset(&profile);
   for (i = 0; i < 500 && !done; ++i)
   {
      done = cia402_profile_step(&profile, &command, 0, 100, 1000, &output);
   }

   check_true("stop completed", done);
   check_int("stop velocity", output.target_velocity, 0);
   check_int("stop mode", output.mode, CIA402_MODE_CSV);
   check_int("stop controlword", output.controlword, 0x010fU);
}

int main(void)
{
   test_absolute_profile();
   test_profile_types();
   test_relative_profile();
   test_jog_velocity_profile();
   test_stop_profile();
   puts("motion_cia402_test: OK");
   return 0;
}
