#include "ethercat_master.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void log_callback(int level, const char *message)
{
   printf("[DLL:%d] %s\n", level, message);
}

static int run_linux_rt_smoke(const char *host, int port)
{
   ECAT_OpenOptions options;
   ECAT_RuntimeStatus runtime;
   ECAT_SlaveInfo slave;
   int position = 0;
   int result;

   result = ECAT_SetBackend(ECAT_BACKEND_LINUX_RT);
   if (result != ECAT_OK)
   {
      printf("ECAT_SetBackend failed: %s (%d)\n", ECAT_ErrorToString(result),
             result);
      return 1;
   }
   result = ECAT_SetLinuxRtEndpoint(host, port);
   if (result != ECAT_OK)
   {
      printf("ECAT_SetLinuxRtEndpoint failed: %s (%d)\n",
             ECAT_ErrorToString(result), result);
      return 1;
   }

   memset(&options, 0, sizeof(options));
   options.period_us = 1000;
   options.request_operational = 1;

   result = ECAT_Open("Linux RT Controller", &options);
   if (result != ECAT_OK)
   {
      printf("ECAT_Open Linux RT failed: %s (%d)\n",
             ECAT_ErrorToString(result), result);
      return 1;
   }

   memset(&runtime, 0, sizeof(runtime));
   ECAT_GetRuntimeStatus(&runtime);
   printf("Linux RT status: %s, slaves=%d, wkc=%d/%d\n",
          runtime.state_text, runtime.slave_count, runtime.last_wkc,
          runtime.expected_wkc);

   if (ECAT_GetSlaveInfo(1, &slave) == ECAT_OK)
   {
      printf("Linux RT slave: %s VID=0x%08X PID=0x%08X DB=%s\n",
             slave.name, slave.vendor_id, slave.product_code,
             slave.database_matched ? slave.database_name : "-");
   }

   result = ECAT_LMS_MoveAbs(1, 12345, 5000);
   printf("ECAT_LMS_MoveAbs result: %s (%d)\n",
          ECAT_ErrorToString(result), result);
   result = ECAT_LMS_GetMoverPosition(1, &position);
   printf("ECAT_LMS_GetMoverPosition result: %s (%d), position=%d\n",
          ECAT_ErrorToString(result), result, position);

   ECAT_Close();
   return 0;
}

int main(int argc, char **argv)
{
   ECAT_AdapterInfo adapters[16];
   int count = 0;
   char db_root[ECAT_MAX_PATH_TEXT];
   char version[32];
   int i;

   ECAT_SetLogCallback(log_callback);
   ECAT_GetVersion(version, sizeof(version));
   printf("EtherCAT DLL version: %s\n", version);
   if (ECAT_DbGetRoot(db_root, sizeof(db_root)) == ECAT_OK)
   {
      int db_count = 0;
      ECAT_DbGetCount(&db_count);
      printf("XML database: %s\n", db_root);
      printf("Registered XML slaves: %d\n", db_count);
   }
   if (argc > 1 && strcmp(argv[1], "--linux-rt") == 0)
   {
      const char *host = argc > 2 ? argv[2] : "127.0.0.1";
      int port = argc > 3 ? atoi(argv[3]) : 15000;
      return run_linux_rt_smoke(host, port);
   }
   if (argc > 1)
   {
      ECAT_DbEntry imported;
      int result = ECAT_DbImportXml(argv[1], &imported);
      printf("XML import result: %s (%d)\n", ECAT_ErrorToString(result),
             result);
      if (result == ECAT_OK)
      {
         ECAT_DbGetCount(&count);
         printf("Imported first: %s VID=0x%08X PID=0x%08X REV=0x%08X\n",
                imported.name, imported.vendor_id, imported.product_code,
                imported.revision);
         printf("Registered XML slaves after import: %d\n", count);
      }
   }

   if (ECAT_ListAdapters(adapters, 16, &count) != ECAT_OK)
   {
      printf("ECAT_ListAdapters failed\n");
      return 1;
   }

   printf("Adapters found: %d\n", count);
   for (i = 0; i < count && i < 16; ++i)
   {
      printf("  %d. %s\n     %s\n", i + 1, adapters[i].name,
             adapters[i].description);
   }

   return 0;
}
