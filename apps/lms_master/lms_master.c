/*
 * LMS EtherCAT master prototype built on SOEM.
 *
 * The application starts as a conservative commissioning tool: adapter list,
 * EtherCAT scan, cyclic process-data monitor, LMS stator map validation, and
 * optional CiA 402 PDO writes when a drive-specific offset map is supplied.
 */

#include "soem/soem.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LMS_MASTER_VERSION "0.1.0"
#define LMS_IO_MAP_SIZE 4096
#define LMS_MAX_STATORS 128
#define LMS_MAX_LINE 512
#define LMS_DEFAULT_PERIOD_US 1000
#define LMS_DEFAULT_CYCLES 1000
#define LMS_PRINT_EVERY 100

typedef enum
{
   MODE_NONE = 0,
   MODE_LIST_ADAPTERS,
   MODE_SCAN,
   MODE_RUN,
   MODE_VALIDATE_CONFIG
} RunMode;

typedef struct
{
   int slave;
   char name[64];
   double start_mm;
   double end_mm;
   double coil_pitch_mm;
   double counts_per_mm;
   int controlword_offset;
   int statusword_offset;
   int mode_offset;
   int mode_display_offset;
   int target_position_offset;
   int actual_position_offset;
} LmsStator;

typedef struct
{
   LmsStator stators[LMS_MAX_STATORS];
   int count;
   double track_start_mm;
   double track_end_mm;
} LmsConfig;

typedef struct
{
   double position_mm;
   double velocity_mm_s;
   double target_mm;
   double max_velocity_mm_s;
   double acceleration_mm_s2;
} MotionState;

typedef struct
{
   ecx_contextt context;
   const char *iface;
   uint8 group;
   int roundtrip_time_us;
   uint8 map[LMS_IO_MAP_SIZE];
} Fieldbus;

typedef struct
{
   RunMode mode;
   const char *iface;
   const char *lms_config_path;
   int period_us;
   int cycles;
   int request_operational;
   int enable_drive_outputs;
   int have_start_mm;
   int have_target_mm;
   double start_mm;
   double target_mm;
   double velocity_mm_s;
   double acceleration_mm_s2;
} Options;

static void print_usage(void);
static int parse_options(int argc, char *argv[], Options *options);
static void list_adapters(void);

static char *trim(char *text);
static int parse_int(const char *text, int *value);
static int parse_double(const char *text, double *value);
static int load_lms_config(const char *path, LmsConfig *config);
static int validate_lms_config(const LmsConfig *config);
static void print_lms_config(const LmsConfig *config);
static const LmsStator *find_stator_for_position(const LmsConfig *config,
                                                 double position_mm);

static void motion_init(MotionState *motion, double start_mm, double target_mm,
                        double max_velocity_mm_s, double acceleration_mm_s2);
static int motion_step(MotionState *motion, double dt_s);

static void fieldbus_initialize(Fieldbus *fieldbus, const char *iface);
static int fieldbus_start(Fieldbus *fieldbus, int request_operational);
static int fieldbus_roundtrip(Fieldbus *fieldbus);
static void fieldbus_stop(Fieldbus *fieldbus);
static void fieldbus_dump_slaves(Fieldbus *fieldbus);
static void fieldbus_check_state(Fieldbus *fieldbus);
static int fieldbus_run(Fieldbus *fieldbus, const Options *options,
                        const LmsConfig *config);

static const char *cia402_state_name(uint16 statusword);
static uint16 cia402_next_controlword(uint16 statusword);
static int lms_apply_motion_outputs(Fieldbus *fieldbus, const LmsConfig *config,
                                    double position_mm,
                                    int enable_drive_outputs);

static double abs_double(double value)
{
   return value < 0.0 ? -value : value;
}

static double min_double(double left, double right)
{
   return left < right ? left : right;
}

static double max_double(double left, double right)
{
   return left > right ? left : right;
}

static int starts_with(const char *text, const char *prefix)
{
   while (*prefix != '\0')
   {
      if (*text++ != *prefix++)
      {
         return 0;
      }
   }
   return 1;
}

static int read_u16_le(const uint8 *src, uint16 *value)
{
   if (src == NULL || value == NULL)
   {
      return 0;
   }
   *value = (uint16)(src[0] | ((uint16)src[1] << 8));
   return 1;
}

static int read_i32_le(const uint8 *src, int32 *value)
{
   uint32 raw;
   if (src == NULL || value == NULL)
   {
      return 0;
   }
   raw = (uint32)src[0] | ((uint32)src[1] << 8) |
         ((uint32)src[2] << 16) | ((uint32)src[3] << 24);
   *value = (int32)raw;
   return 1;
}

static void write_u8(uint8 *dst, uint8 value)
{
   dst[0] = value;
}

static void write_u16_le(uint8 *dst, uint16 value)
{
   dst[0] = (uint8)(value & 0x00ff);
   dst[1] = (uint8)((value >> 8) & 0x00ff);
}

static void write_i32_le(uint8 *dst, int32 value)
{
   uint32 raw = (uint32)value;
   dst[0] = (uint8)(raw & 0x000000ffu);
   dst[1] = (uint8)((raw >> 8) & 0x000000ffu);
   dst[2] = (uint8)((raw >> 16) & 0x000000ffu);
   dst[3] = (uint8)((raw >> 24) & 0x000000ffu);
}

static int output_range_ok(const ec_slavet *slave, int offset, int size)
{
   if (slave == NULL || slave->outputs == NULL || offset < 0 || size <= 0)
   {
      return 0;
   }
   return (uint32)(offset + size) <= slave->Obytes;
}

static int input_range_ok(const ec_slavet *slave, int offset, int size)
{
   if (slave == NULL || slave->inputs == NULL || offset < 0 || size <= 0)
   {
      return 0;
   }
   return (uint32)(offset + size) <= slave->Ibytes;
}

int main(int argc, char *argv[])
{
   Options options;
   LmsConfig config;
   Fieldbus fieldbus;
   int have_config = 0;
   int exit_code = 1;

   memset(&options, 0, sizeof(options));
   memset(&config, 0, sizeof(config));
   options.period_us = LMS_DEFAULT_PERIOD_US;
   options.cycles = LMS_DEFAULT_CYCLES;
   options.request_operational = 1;
   options.velocity_mm_s = 250.0;
   options.acceleration_mm_s2 = 1000.0;

   if (!parse_options(argc, argv, &options))
   {
      print_usage();
      return 1;
   }

   if (options.mode == MODE_NONE)
   {
      print_usage();
      printf("\nAvailable adapters:\n");
      list_adapters();
      return 1;
   }

   if (options.lms_config_path != NULL)
   {
      if (!load_lms_config(options.lms_config_path, &config))
      {
         return 1;
      }
      if (!validate_lms_config(&config))
      {
         return 1;
      }
      have_config = 1;
      print_lms_config(&config);
   }

   if (options.mode == MODE_LIST_ADAPTERS)
   {
      list_adapters();
      return 0;
   }

   if (options.mode == MODE_VALIDATE_CONFIG)
   {
      if (!have_config)
      {
         printf("ERROR: --validate-config requires --lms-config <file>\n");
         return 1;
      }
      return 0;
   }

   if (options.iface == NULL)
   {
      printf("ERROR: --iface <adapter> is required for --scan or --run\n\n");
      print_usage();
      return 1;
   }

   fieldbus_initialize(&fieldbus, options.iface);
   if (!fieldbus_start(&fieldbus, options.mode == MODE_RUN &&
                                      options.request_operational))
   {
      fieldbus_stop(&fieldbus);
      return 1;
   }

   fieldbus_dump_slaves(&fieldbus);

   if (options.mode == MODE_SCAN)
   {
      exit_code = 0;
   }
   else if (options.mode == MODE_RUN)
   {
      exit_code = fieldbus_run(&fieldbus, &options,
                               have_config ? &config : NULL)
                     ? 0
                     : 1;
   }

   fieldbus_stop(&fieldbus);
   return exit_code;
}

static void print_usage(void)
{
   printf("LMS EtherCAT Master %s (SOEM based)\n", LMS_MASTER_VERSION);
   printf("\nUsage:\n");
   printf("  lms_master --list-adapters\n");
   printf("  lms_master --iface <adapter> --scan\n");
   printf("  lms_master --iface <adapter> --run [--period-us 1000] [--cycles 1000]\n");
   printf("  lms_master --lms-config config/lms_bosch_sample.csv --validate-config\n");
   printf("  lms_master --iface <adapter> --run --lms-config <csv> --demo-target-mm 500\n");
   printf("\nMotion and safety options:\n");
   printf("  --safe-op-only              Do not request EtherCAT OP state during --run\n");
   printf("  --enable-drive-outputs      Write configured CiA 402 PDO outputs\n");
   printf("  --start-mm <mm>             Initial virtual mover position\n");
   printf("  --demo-target-mm <mm>       Target for the built-in trapezoid planner\n");
   printf("  --demo-velocity-mm-s <v>    Planner max velocity, default 250\n");
   printf("  --demo-accel-mm-s2 <a>      Planner acceleration, default 1000\n");
   printf("\nThe application never writes motion PDOs unless --enable-drive-outputs is set.\n");
}

static int parse_options(int argc, char *argv[], Options *options)
{
   int i;
   for (i = 1; i < argc; ++i)
   {
      const char *arg = argv[i];
      if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
      {
         options->mode = MODE_NONE;
         return 1;
      }
      else if (strcmp(arg, "--list-adapters") == 0)
      {
         options->mode = MODE_LIST_ADAPTERS;
      }
      else if (strcmp(arg, "--scan") == 0)
      {
         options->mode = MODE_SCAN;
      }
      else if (strcmp(arg, "--run") == 0)
      {
         options->mode = MODE_RUN;
      }
      else if (strcmp(arg, "--validate-config") == 0)
      {
         options->mode = MODE_VALIDATE_CONFIG;
      }
      else if (strcmp(arg, "--iface") == 0 && i + 1 < argc)
      {
         options->iface = argv[++i];
      }
      else if (strcmp(arg, "--lms-config") == 0 && i + 1 < argc)
      {
         options->lms_config_path = argv[++i];
      }
      else if (strcmp(arg, "--period-us") == 0 && i + 1 < argc)
      {
         if (!parse_int(argv[++i], &options->period_us) ||
             options->period_us <= 0)
         {
            printf("ERROR: invalid --period-us\n");
            return 0;
         }
      }
      else if (strcmp(arg, "--cycles") == 0 && i + 1 < argc)
      {
         if (!parse_int(argv[++i], &options->cycles))
         {
            printf("ERROR: invalid --cycles\n");
            return 0;
         }
      }
      else if (strcmp(arg, "--safe-op-only") == 0)
      {
         options->request_operational = 0;
      }
      else if (strcmp(arg, "--enable-drive-outputs") == 0)
      {
         options->enable_drive_outputs = 1;
      }
      else if (strcmp(arg, "--start-mm") == 0 && i + 1 < argc)
      {
         if (!parse_double(argv[++i], &options->start_mm))
         {
            printf("ERROR: invalid --start-mm\n");
            return 0;
         }
         options->have_start_mm = 1;
      }
      else if (strcmp(arg, "--demo-target-mm") == 0 && i + 1 < argc)
      {
         if (!parse_double(argv[++i], &options->target_mm))
         {
            printf("ERROR: invalid --demo-target-mm\n");
            return 0;
         }
         options->have_target_mm = 1;
      }
      else if (strcmp(arg, "--demo-velocity-mm-s") == 0 && i + 1 < argc)
      {
         if (!parse_double(argv[++i], &options->velocity_mm_s) ||
             options->velocity_mm_s <= 0.0)
         {
            printf("ERROR: invalid --demo-velocity-mm-s\n");
            return 0;
         }
      }
      else if (strcmp(arg, "--demo-accel-mm-s2") == 0 && i + 1 < argc)
      {
         if (!parse_double(argv[++i], &options->acceleration_mm_s2) ||
             options->acceleration_mm_s2 <= 0.0)
         {
            printf("ERROR: invalid --demo-accel-mm-s2\n");
            return 0;
         }
      }
      else
      {
         printf("ERROR: unknown or incomplete option: %s\n", arg);
         return 0;
      }
   }
   return 1;
}

static void list_adapters(void)
{
   ec_adaptert *adapter = NULL;
   ec_adaptert *head = NULL;

   head = adapter = ec_find_adapters();
   if (adapter == NULL)
   {
      printf("  No adapters reported by pcap. Check Npcap/WinPcap installation.\n");
      return;
   }

   while (adapter != NULL)
   {
      printf("  %s\n      %s\n", adapter->name, adapter->desc);
      adapter = adapter->next;
   }
   ec_free_adapters(head);
}

static char *trim(char *text)
{
   char *end;
   while (*text != '\0' && isspace((unsigned char)*text))
   {
      ++text;
   }
   if (*text == '\0')
   {
      return text;
   }
   end = text + strlen(text) - 1;
   while (end > text && isspace((unsigned char)*end))
   {
      *end-- = '\0';
   }
   return text;
}

static int parse_int(const char *text, int *value)
{
   char *end = NULL;
   long parsed;
   errno = 0;
   parsed = strtol(text, &end, 10);
   if (errno != 0 || end == text || *trim(end) != '\0' ||
       parsed < INT_MIN || parsed > INT_MAX)
   {
      return 0;
   }
   *value = (int)parsed;
   return 1;
}

static int parse_double(const char *text, double *value)
{
   char *end = NULL;
   double parsed;
   errno = 0;
   parsed = strtod(text, &end);
   if (errno != 0 || end == text || *trim(end) != '\0')
   {
      return 0;
   }
   *value = parsed;
   return 1;
}

static int parse_csv_line(char *line, char *fields[], int max_fields)
{
   int count = 0;
   char *cursor = line;

   while (count < max_fields)
   {
      char *comma = strchr(cursor, ',');
      if (comma != NULL)
      {
         *comma = '\0';
      }
      fields[count++] = trim(cursor);
      if (comma == NULL)
      {
         break;
      }
      cursor = comma + 1;
   }

   return count;
}

static int parse_optional_int(char *field, int default_value, int *value)
{
   field = trim(field);
   if (*field == '\0')
   {
      *value = default_value;
      return 1;
   }
   return parse_int(field, value);
}

static int load_lms_config(const char *path, LmsConfig *config)
{
   FILE *file;
   char line[LMS_MAX_LINE];
   int line_no = 0;

   memset(config, 0, sizeof(*config));
   config->track_start_mm = 0.0;
   config->track_end_mm = 0.0;

   file = fopen(path, "r");
   if (file == NULL)
   {
      printf("ERROR: cannot open LMS config: %s\n", path);
      return 0;
   }

   while (fgets(line, sizeof(line), file) != NULL)
   {
      char *fields[12];
      char *text;
      LmsStator stator;
      int field_count;

      ++line_no;
      text = trim(line);
      if (*text == '\0' || *text == '#')
      {
         continue;
      }

      memset(&stator, 0, sizeof(stator));
      stator.controlword_offset = -1;
      stator.statusword_offset = -1;
      stator.mode_offset = -1;
      stator.mode_display_offset = -1;
      stator.target_position_offset = -1;
      stator.actual_position_offset = -1;

      field_count = parse_csv_line(text, fields, 12);
      if (field_count < 6)
      {
         printf("ERROR: %s:%d requires at least 6 CSV fields\n", path, line_no);
         fclose(file);
         return 0;
      }

      if (!parse_int(fields[0], &stator.slave))
      {
         if (line_no == 1 || starts_with(trim(fields[0]), "slave"))
         {
            continue;
         }
         printf("ERROR: %s:%d invalid slave number\n", path, line_no);
         fclose(file);
         return 0;
      }

      (void)snprintf(stator.name, sizeof(stator.name), "%s", trim(fields[1]));
      if (!parse_double(fields[2], &stator.start_mm) ||
          !parse_double(fields[3], &stator.end_mm) ||
          !parse_double(fields[4], &stator.coil_pitch_mm) ||
          !parse_double(fields[5], &stator.counts_per_mm))
      {
         printf("ERROR: %s:%d invalid numeric stator field\n", path, line_no);
         fclose(file);
         return 0;
      }

      if (field_count > 6 &&
          !parse_optional_int(fields[6], -1, &stator.controlword_offset))
      {
         printf("ERROR: %s:%d invalid controlword_offset\n", path, line_no);
         fclose(file);
         return 0;
      }
      if (field_count > 7 &&
          !parse_optional_int(fields[7], -1, &stator.statusword_offset))
      {
         printf("ERROR: %s:%d invalid statusword_offset\n", path, line_no);
         fclose(file);
         return 0;
      }
      if (field_count > 8 && !parse_optional_int(fields[8], -1,
                                                 &stator.mode_offset))
      {
         printf("ERROR: %s:%d invalid mode_offset\n", path, line_no);
         fclose(file);
         return 0;
      }
      if (field_count > 9 && !parse_optional_int(fields[9], -1,
                                                 &stator.mode_display_offset))
      {
         printf("ERROR: %s:%d invalid mode_display_offset\n", path, line_no);
         fclose(file);
         return 0;
      }
      if (field_count > 10 && !parse_optional_int(fields[10], -1,
                                                  &stator.target_position_offset))
      {
         printf("ERROR: %s:%d invalid target_position_offset\n", path, line_no);
         fclose(file);
         return 0;
      }
      if (field_count > 11 && !parse_optional_int(fields[11], -1,
                                                  &stator.actual_position_offset))
      {
         printf("ERROR: %s:%d invalid actual_position_offset\n", path, line_no);
         fclose(file);
         return 0;
      }

      if (config->count >= LMS_MAX_STATORS)
      {
         printf("ERROR: too many stators. Increase LMS_MAX_STATORS.\n");
         fclose(file);
         return 0;
      }

      config->stators[config->count++] = stator;
   }

   fclose(file);
   return 1;
}

static int validate_lms_config(const LmsConfig *config)
{
   int i, j;
   if (config->count <= 0)
   {
      printf("ERROR: LMS config has no stators\n");
      return 0;
   }

   for (i = 0; i < config->count; ++i)
   {
      const LmsStator *stator = &config->stators[i];
      if (stator->slave <= 0)
      {
         printf("ERROR: stator %d has invalid EtherCAT slave index\n", i + 1);
         return 0;
      }
      if (stator->end_mm <= stator->start_mm)
      {
         printf("ERROR: stator %s end_mm must be greater than start_mm\n",
                stator->name);
         return 0;
      }
      if (stator->coil_pitch_mm <= 0.0)
      {
         printf("ERROR: stator %s coil_pitch_mm must be positive\n",
                stator->name);
         return 0;
      }
      if (stator->counts_per_mm <= 0.0)
      {
         printf("ERROR: stator %s counts_per_mm must be positive\n",
                stator->name);
         return 0;
      }

      for (j = i + 1; j < config->count; ++j)
      {
         const LmsStator *other = &config->stators[j];
         if (stator->start_mm < other->end_mm &&
             other->start_mm < stator->end_mm)
         {
            printf("ERROR: stator ranges overlap: %s and %s\n",
                   stator->name, other->name);
            return 0;
         }
      }
   }

   printf("LMS config validation: OK (%d stators)\n", config->count);
   return 1;
}

static void print_lms_config(const LmsConfig *config)
{
   int i;
   double start_mm;
   double end_mm;

   if (config->count <= 0)
   {
      return;
   }

   start_mm = config->stators[0].start_mm;
   end_mm = config->stators[0].end_mm;
   for (i = 1; i < config->count; ++i)
   {
      start_mm = min_double(start_mm, config->stators[i].start_mm);
      end_mm = max_double(end_mm, config->stators[i].end_mm);
   }

   printf("\nLMS stator map: %.3f mm .. %.3f mm (%d stators)\n",
          start_mm, end_mm, config->count);
   for (i = 0; i < config->count; ++i)
   {
      const LmsStator *stator = &config->stators[i];
      printf("  slave %d %-16s range %.3f..%.3f mm pitch %.3f mm "
             "counts/mm %.3f\n",
             stator->slave, stator->name, stator->start_mm, stator->end_mm,
             stator->coil_pitch_mm, stator->counts_per_mm);
   }
}

static const LmsStator *find_stator_for_position(const LmsConfig *config,
                                                 double position_mm)
{
   int i;
   if (config == NULL)
   {
      return NULL;
   }

   for (i = 0; i < config->count; ++i)
   {
      const LmsStator *stator = &config->stators[i];
      if (position_mm >= stator->start_mm && position_mm < stator->end_mm)
      {
         return stator;
      }
   }
   return NULL;
}

static void motion_init(MotionState *motion, double start_mm, double target_mm,
                        double max_velocity_mm_s, double acceleration_mm_s2)
{
   memset(motion, 0, sizeof(*motion));
   motion->position_mm = start_mm;
   motion->target_mm = target_mm;
   motion->max_velocity_mm_s = max_velocity_mm_s;
   motion->acceleration_mm_s2 = acceleration_mm_s2;
}

static int motion_step(MotionState *motion, double dt_s)
{
   double error = motion->target_mm - motion->position_mm;
   double direction;
   double stop_distance;
   double next_position;

   if (abs_double(error) < 0.000001 && abs_double(motion->velocity_mm_s) < 0.000001)
   {
      motion->position_mm = motion->target_mm;
      motion->velocity_mm_s = 0.0;
      return 1;
   }

   direction = error >= 0.0 ? 1.0 : -1.0;
   stop_distance = (motion->velocity_mm_s * motion->velocity_mm_s) /
                   (2.0 * motion->acceleration_mm_s2);

   if (abs_double(error) <= stop_distance)
   {
      motion->velocity_mm_s -= direction * motion->acceleration_mm_s2 * dt_s;
   }
   else
   {
      motion->velocity_mm_s += direction * motion->acceleration_mm_s2 * dt_s;
   }

   if (motion->velocity_mm_s > motion->max_velocity_mm_s)
   {
      motion->velocity_mm_s = motion->max_velocity_mm_s;
   }
   else if (motion->velocity_mm_s < -motion->max_velocity_mm_s)
   {
      motion->velocity_mm_s = -motion->max_velocity_mm_s;
   }

   next_position = motion->position_mm + motion->velocity_mm_s * dt_s;
   if ((direction > 0.0 && next_position > motion->target_mm) ||
       (direction < 0.0 && next_position < motion->target_mm))
   {
      motion->position_mm = motion->target_mm;
      motion->velocity_mm_s = 0.0;
      return 1;
   }

   motion->position_mm = next_position;
   return 0;
}

static void fieldbus_initialize(Fieldbus *fieldbus, const char *iface)
{
   memset(fieldbus, 0, sizeof(*fieldbus));
   fieldbus->iface = iface;
   fieldbus->group = 0;
}

static int fieldbus_roundtrip(Fieldbus *fieldbus)
{
   ec_timet start;
   ec_timet end;
   ec_timet diff;
   int wkc;

   start = osal_current_time();
   ecx_send_processdata(&fieldbus->context);
   wkc = ecx_receive_processdata(&fieldbus->context, EC_TIMEOUTRET);
   end = osal_current_time();
   osal_time_diff(&start, &end, &diff);
   fieldbus->roundtrip_time_us =
      (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);
   return wkc;
}

static int fieldbus_start(Fieldbus *fieldbus, int request_operational)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;
   ec_slavet *slave;
   int i;

   printf("Initializing SOEM on '%s'... ", fieldbus->iface);
   if (!ecx_init(context, fieldbus->iface))
   {
      printf("no socket connection\n");
      return 0;
   }
   printf("done\n");

   printf("Finding EtherCAT slaves... ");
   if (ecx_config_init(context) <= 0)
   {
      printf("no slaves found\n");
      return 0;
   }
   printf("%d slaves found\n", context->slavecount);

   printf("Mapping process data... ");
   ecx_config_map_group(context, fieldbus->map, fieldbus->group);
   printf("%luO + %luI bytes, %u segments\n",
          (unsigned long)grp->Obytes, (unsigned long)grp->Ibytes,
          (unsigned int)grp->nsegments);

   printf("Configuring distributed clocks... ");
   ecx_configdc(context);
   printf("done\n");

   printf("Waiting for SAFE_OP... ");
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   printf("done\n");

   (void)fieldbus_roundtrip(fieldbus);

   if (!request_operational)
   {
      printf("Staying in SAFE_OP as requested\n");
      return 1;
   }

   printf("Requesting OPERATIONAL state");
   slave = context->slavelist;
   slave->state = EC_STATE_OPERATIONAL;
   ecx_writestate(context, 0);

   for (i = 0; i < 40; ++i)
   {
      printf(".");
      (void)fieldbus_roundtrip(fieldbus);
      ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 20);
      if (slave->state == EC_STATE_OPERATIONAL)
      {
         printf(" done\n");
         return 1;
      }
   }

   printf(" failed\n");
   ecx_readstate(context);
   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->state != EC_STATE_OPERATIONAL)
      {
         printf("  slave %d state=0x%04x AL=0x%04x %s\n", i, slave->state,
                slave->ALstatuscode,
                ec_ALstatuscode2string(slave->ALstatuscode));
      }
   }
   return 0;
}

static void fieldbus_stop(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_slavet *slave = context->slavelist;

   if (context->slavecount > 0)
   {
      printf("Requesting INIT on all slaves... ");
      slave->state = EC_STATE_INIT;
      ecx_writestate(context, 0);
      printf("done\n");
   }

   printf("Closing SOEM socket... ");
   ecx_close(context);
   printf("done\n");
}

static void fieldbus_dump_slaves(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   int i;

   printf("\nEtherCAT slaves:\n");
   for (i = 1; i <= context->slavecount; ++i)
   {
      const ec_slavet *slave = context->slavelist + i;
      printf("  %02d %-40s state=0x%04x O=%luB/%ub I=%luB/%ub "
             "man=0x%08lx id=0x%08lx rev=0x%08lx\n",
             i, slave->name, slave->state, (unsigned long)slave->Obytes,
             (unsigned int)slave->Obits, (unsigned long)slave->Ibytes,
             (unsigned int)slave->Ibits, (unsigned long)slave->eep_man,
             (unsigned long)slave->eep_id, (unsigned long)slave->eep_rev);
   }
   printf("\n");
}

static void fieldbus_check_state(Fieldbus *fieldbus)
{
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;
   ec_slavet *slave;
   int i;

   grp->docheckstate = FALSE;
   ecx_readstate(context);

   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->group != fieldbus->group)
      {
         continue;
      }

      if (slave->state != EC_STATE_OPERATIONAL)
      {
         grp->docheckstate = TRUE;
         if (slave->state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
         {
            printf("* Slave %d SAFE_OP+ERROR, ACK\n", i);
            slave->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ecx_writestate(context, i);
         }
         else if (slave->state == EC_STATE_SAFE_OP)
         {
            printf("* Slave %d SAFE_OP, request OP\n", i);
            slave->state = EC_STATE_OPERATIONAL;
            ecx_writestate(context, i);
         }
         else if (slave->state > EC_STATE_NONE)
         {
            if (ecx_reconfig_slave(context, i, EC_TIMEOUTRET))
            {
               slave->islost = FALSE;
               printf("* Slave %d reconfigured\n", i);
            }
         }
         else if (!slave->islost)
         {
            ecx_statecheck(context, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
            if (slave->state == EC_STATE_NONE)
            {
               slave->islost = TRUE;
               printf("* Slave %d lost\n", i);
            }
         }
      }
      else if (slave->islost)
      {
         if (slave->state != EC_STATE_NONE)
         {
            slave->islost = FALSE;
            printf("* Slave %d found\n", i);
         }
         else if (ecx_recover_slave(context, i, EC_TIMEOUTRET))
         {
            slave->islost = FALSE;
            printf("* Slave %d recovered\n", i);
         }
      }
   }
}

static int fieldbus_run(Fieldbus *fieldbus, const Options *options,
                        const LmsConfig *config)
{
   ec_groupt *grp = fieldbus->context.grouplist + fieldbus->group;
   int expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
   int cycle = 0;
   int finished = 0;
   MotionState motion;
   int have_motion = 0;

   if (config != NULL && options->have_target_mm)
   {
      double start_mm = options->have_start_mm ? options->start_mm
                                               : config->stators[0].start_mm;
      motion_init(&motion, start_mm, options->target_mm,
                  options->velocity_mm_s, options->acceleration_mm_s2);
      have_motion = 1;
      printf("Motion planner: start %.3f mm -> target %.3f mm, "
             "vmax %.3f mm/s, accel %.3f mm/s^2\n",
             motion.position_mm, motion.target_mm, motion.max_velocity_mm_s,
             motion.acceleration_mm_s2);
   }

   printf("Cyclic run: period=%d us cycles=%d expectedWKC=%d driveOutputs=%s\n",
          options->period_us, options->cycles, expected_wkc,
          options->enable_drive_outputs ? "enabled" : "disabled");

   while (options->cycles < 0 || cycle < options->cycles)
   {
      int wkc;
      const LmsStator *active = NULL;

      ++cycle;
      if (have_motion && !finished)
      {
         finished = motion_step(&motion, (double)options->period_us / 1000000.0);
      }

      if (have_motion)
      {
         active = find_stator_for_position(config, motion.position_mm);
         if (active == NULL)
         {
            printf("ERROR: mover position %.3f mm is outside LMS stator map\n",
                   motion.position_mm);
            return 0;
         }

         if (!lms_apply_motion_outputs(fieldbus, config, motion.position_mm,
                                       options->enable_drive_outputs))
         {
            return 0;
         }
      }

      wkc = fieldbus_roundtrip(fieldbus);
      if (wkc < expected_wkc)
      {
         printf("Cycle %d WKC %d below expected %d\n", cycle, wkc,
                expected_wkc);
         fieldbus_check_state(fieldbus);
      }

      if (cycle == 1 || cycle % LMS_PRINT_EVERY == 0 || finished)
      {
         if (have_motion)
         {
            printf("Cycle %d rt=%dus wkc=%d pos=%.3fmm vel=%.3fmm/s "
                   "activeSlave=%d %s\n",
                   cycle, fieldbus->roundtrip_time_us, wkc, motion.position_mm,
                   motion.velocity_mm_s, active->slave, active->name);
         }
         else
         {
            printf("Cycle %d rt=%dus wkc=%d DC=%lld\n", cycle,
                   fieldbus->roundtrip_time_us, wkc,
                   (long long)fieldbus->context.DCtime);
         }
      }

      if (finished)
      {
         printf("Motion planner reached target at cycle %d\n", cycle);
         break;
      }

      osal_usleep((uint32)options->period_us);
   }

   return 1;
}

static const char *cia402_state_name(uint16 statusword)
{
   if ((statusword & 0x004f) == 0x0000)
   {
      return "Not ready";
   }
   if ((statusword & 0x004f) == 0x0040)
   {
      return "Switch on disabled";
   }
   if ((statusword & 0x006f) == 0x0021)
   {
      return "Ready to switch on";
   }
   if ((statusword & 0x006f) == 0x0023)
   {
      return "Switched on";
   }
   if ((statusword & 0x006f) == 0x0027)
   {
      return "Operation enabled";
   }
   if ((statusword & 0x006f) == 0x0007)
   {
      return "Quick stop active";
   }
   if ((statusword & 0x004f) == 0x000f)
   {
      return "Fault reaction";
   }
   if ((statusword & 0x004f) == 0x0008)
   {
      return "Fault";
   }
   return "Unknown";
}

static uint16 cia402_next_controlword(uint16 statusword)
{
   if ((statusword & 0x004f) == 0x0008)
   {
      return 0x0080;
   }
   if ((statusword & 0x004f) == 0x0040)
   {
      return 0x0006;
   }
   if ((statusword & 0x006f) == 0x0021)
   {
      return 0x0007;
   }
   if ((statusword & 0x006f) == 0x0023)
   {
      return 0x000f;
   }
   if ((statusword & 0x006f) == 0x0027)
   {
      return 0x000f;
   }
   return 0x0006;
}

static int lms_apply_motion_outputs(Fieldbus *fieldbus, const LmsConfig *config,
                                    double position_mm,
                                    int enable_drive_outputs)
{
   const LmsStator *active = find_stator_for_position(config, position_mm);
   ecx_contextt *context = &fieldbus->context;
   ec_slavet *slave;
   double local_mm;
   int32 target_counts;
   int32 actual_counts = 0;
   uint16 statusword = 0;
   uint16 controlword = 0;
   int have_actual_counts = 0;

   if (active == NULL)
   {
      return 0;
   }

   if (active->slave <= 0 || active->slave > context->slavecount)
   {
      printf("ERROR: LMS stator %s references missing slave %d\n",
             active->name, active->slave);
      return 0;
   }

   slave = context->slavelist + active->slave;
   local_mm = position_mm - active->start_mm;
   target_counts = (int32)(local_mm * active->counts_per_mm);

   if (!enable_drive_outputs)
   {
      return 1;
   }

   if (!output_range_ok(slave, active->controlword_offset, 2) ||
       !output_range_ok(slave, active->target_position_offset, 4))
   {
      printf("ERROR: stator %s is missing valid PDO output offsets. "
             "Disable --enable-drive-outputs or update the LMS CSV.\n",
             active->name);
      return 0;
   }

   if (input_range_ok(slave, active->statusword_offset, 2))
   {
      (void)read_u16_le(slave->inputs + active->statusword_offset, &statusword);
      controlword = cia402_next_controlword(statusword);
   }
   else
   {
      controlword = 0x0006;
   }

   write_u16_le(slave->outputs + active->controlword_offset, controlword);
   write_i32_le(slave->outputs + active->target_position_offset, target_counts);

   if (output_range_ok(slave, active->mode_offset, 1))
   {
      write_u8(slave->outputs + active->mode_offset, 8u);
   }

   if (input_range_ok(slave, active->actual_position_offset, 4))
   {
      have_actual_counts = read_i32_le(slave->inputs + active->actual_position_offset,
                                       &actual_counts);
   }

   if (input_range_ok(slave, active->statusword_offset, 2))
   {
      if (have_actual_counts)
      {
         printf("CiA402 slave %d %s status=0x%04x %-20s control=0x%04x "
                "target=%ld actual=%ld counts\r",
                active->slave, active->name, statusword,
                cia402_state_name(statusword), controlword,
                (long)target_counts, (long)actual_counts);
      }
      else
      {
         printf("CiA402 slave %d %s status=0x%04x %-20s control=0x%04x "
                "target=%ld counts\r",
                active->slave, active->name, statusword,
                cia402_state_name(statusword), controlword,
                (long)target_counts);
      }
   }

   return 1;
}
