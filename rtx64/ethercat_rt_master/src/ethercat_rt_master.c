#include "ecat_rt_ipc.h"

#include <stdio.h>
#include <string.h>

#define ECAT_RT_DEFAULT_PERIOD_US 1000

typedef struct EcatRtCore
{
   int period_us;
   int running;
   int stop_requested;
   ECAT_RtSharedMemory shared;
} EcatRtCore;

static EcatRtCore G_rt;

static void rt_set_text(char *dst, size_t dst_size, const char *text)
{
   if (dst_size == 0)
   {
      return;
   }
   if (text == NULL)
   {
      text = "";
   }
   (void)snprintf(dst, dst_size, "%s", text);
}

static void rt_init_status(void)
{
   memset(&G_rt.shared, 0, sizeof(G_rt.shared));
   G_rt.shared.version = ECAT_RT_IPC_VERSION;
   G_rt.shared.status.version = ECAT_RT_IPC_VERSION;
   G_rt.shared.status.period_us = G_rt.period_us;
   rt_set_text(G_rt.shared.status.state_text,
               sizeof(G_rt.shared.status.state_text),
               "RTX64 master scaffold");
}

static void rt_process_command(const ECAT_RtCommand *command)
{
   if (command == NULL)
   {
      return;
   }

   G_rt.shared.status.last_command_sequence = command->sequence;
   switch ((ECAT_RtCommandType)command->type)
   {
   case ECAT_RT_CMD_START:
      G_rt.running = 1;
      rt_set_text(G_rt.shared.status.state_text,
                  sizeof(G_rt.shared.status.state_text), "Start requested");
      break;
   case ECAT_RT_CMD_STOP:
      G_rt.running = 0;
      G_rt.stop_requested = 1;
      rt_set_text(G_rt.shared.status.state_text,
                  sizeof(G_rt.shared.status.state_text), "Stop requested");
      break;
   case ECAT_RT_CMD_MOVE_ABS:
   case ECAT_RT_CMD_MOVE_VEL:
   case ECAT_RT_CMD_HOME:
   case ECAT_RT_CMD_HALT:
   case ECAT_RT_CMD_LMS_MOVE_ABS:
   case ECAT_RT_CMD_LMS_MOVE_VEL:
   case ECAT_RT_CMD_LMS_STOP:
      rt_set_text(G_rt.shared.status.state_text,
                  sizeof(G_rt.shared.status.state_text),
                  "Motion command queued");
      break;
   default:
      break;
   }
}

static void rt_cycle_once(void)
{
   ++G_rt.shared.status.status_sequence;
   G_rt.shared.status.running = G_rt.running;

   /*
    * Phase 2 implementation point:
    * 1. send EtherCAT process data through RTX64 real-time NIC/NAL
    * 2. receive process data
    * 3. update WKC/DC/cycle statistics
    * 4. run CiA402 and LMS command state machines
    * 5. publish ECAT_RtStatus to shared memory
    */
}

int main(int argc, char **argv)
{
   int cycle_limit = 10;
   int i;

   (void)argc;
   (void)argv;

   memset(&G_rt, 0, sizeof(G_rt));
   G_rt.period_us = ECAT_RT_DEFAULT_PERIOD_US;
   rt_init_status();

   printf("ethercat_rt_master scaffold started\n");
   printf("RTX64 SDK/NAL EtherCAT transport will be connected in phase 2.\n");

   for (i = 0; i < cycle_limit && !G_rt.stop_requested; ++i)
   {
      rt_process_command(&G_rt.shared.command);
      rt_cycle_once();
   }

   printf("ethercat_rt_master scaffold stopped\n");
   return 0;
}
