#include <ecrt.h>

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LTS_VENDOR_ID 0x0000205eu
#define LTS_PRODUCT_CODE 0x90000300u
#define DEFAULT_PERIOD_US 1000
#define DEFAULT_PRINT_EVERY 1000
#define DEFAULT_SEQUENCE_TIMEOUT_MS 5000

typedef struct LtsOffsets
{
   unsigned int controlword;
   unsigned int target_position;
   unsigned int target_velocity;
   unsigned int mode_of_operation;
   unsigned int statusword;
   unsigned int actual_position;
   unsigned int actual_velocity;
   unsigned int mode_display;
} LtsOffsets;

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

typedef struct MonitorOptions
{
   unsigned int master_index;
   unsigned int alias;
   unsigned int position;
   uint32_t vendor_id;
   uint32_t product_code;
   int period_us;
   int print_every;
   int max_cycles;
   uint16_t controlword;
   int32_t target_position;
   int32_t target_velocity;
   int8_t mode;
   int priority;
   int configure_pdos;
   Cia402Sequence sequence;
   int sequence_timeout_ms;
} MonitorOptions;

static volatile sig_atomic_t G_stop;

static ec_pdo_entry_info_t G_lts_pdo_entries[] = {
   {0x6040, 0x00, 16},
   {0x607a, 0x00, 32},
   {0x60ff, 0x00, 32},
   {0x6060, 0x00, 8},
   {0x0000, 0x00, 24},
   {0x6041, 0x00, 16},
   {0x6064, 0x00, 32},
   {0x606c, 0x00, 32},
   {0x6061, 0x00, 8},
   {0x0000, 0x00, 24},
};

static ec_pdo_info_t G_lts_pdos[] = {
   {0x1600, 5, &G_lts_pdo_entries[0]},
   {0x1a00, 5, &G_lts_pdo_entries[5]},
};

static ec_sync_info_t G_lts_syncs[] = {
   {2, EC_DIR_OUTPUT, 1, &G_lts_pdos[0], EC_WD_ENABLE},
   {3, EC_DIR_INPUT, 1, &G_lts_pdos[1], EC_WD_DISABLE},
   {0xff, EC_DIR_INVALID, 0, NULL, EC_WD_DEFAULT}
};

static void on_signal(int sig)
{
   (void)sig;
   G_stop = 1;
}

static int parse_u32(const char *text, uint32_t *value)
{
   char *end = NULL;
   unsigned long parsed;

   if (text == NULL || value == NULL)
   {
      return -1;
   }

   errno = 0;
   parsed = strtoul(text, &end, 0);
   if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
   {
      return -1;
   }

   *value = (uint32_t)parsed;
   return 0;
}

static int parse_i32(const char *text, int32_t *value)
{
   char *end = NULL;
   long parsed;

   if (text == NULL || value == NULL)
   {
      return -1;
   }

   errno = 0;
   parsed = strtol(text, &end, 0);
   if (errno != 0 || end == text || *end != '\0' ||
       parsed < INT32_MIN || parsed > INT32_MAX)
   {
      return -1;
   }

   *value = (int32_t)parsed;
   return 0;
}

static int parse_int(const char *text, int *value)
{
   int32_t parsed;

   if (parse_i32(text, &parsed) != 0)
   {
      return -1;
   }
   *value = (int)parsed;
   return 0;
}

static void usage(const char *argv0)
{
   printf("Usage: %s [options]\n", argv0);
   printf("\n");
   printf("Safe LTS_MotorDriver1x IgH cyclic PDO monitor.\n");
   printf("Default outputs are zero: controlword=0, target=0, velocity=0, mode=0.\n");
   printf("\n");
   printf("Options:\n");
   printf("  --period-us N        Cycle period in us. Default: %d\n", DEFAULT_PERIOD_US);
   printf("  --print-every N      Print once every N cycles. Default: %d\n", DEFAULT_PRINT_EVERY);
   printf("  --cycles N           Stop after N cycles. 0 means run until Ctrl+C.\n");
   printf("  --position N         Slave ring position. Default: 0\n");
   printf("  --vendor HEX         Vendor ID. Default: 0x%08x\n", LTS_VENDOR_ID);
   printf("  --product HEX        Product code. Default: 0x%08x\n", LTS_PRODUCT_CODE);
   printf("  --controlword HEX    Output controlword. Default: 0x0000\n");
   printf("  --target-position N  Output target position. Default: 0\n");
   printf("  --target-velocity N  Output target velocity. Default: 0\n");
   printf("  --mode N             Output CiA402 mode. Default: 0\n");
   printf("  --priority N         SCHED_FIFO priority. Default: 80\n");
   printf("  --use-sii-pdos       Use the slave/default SII PDO map without rewriting it.\n");
   printf("  --fault-reset        Run CiA402 fault reset sequence.\n");
   printf("  --cia402-shutdown    Run CiA402 shutdown sequence to ReadyToSwitchOn.\n");
   printf("  --cia402-switch-on   Run CiA402 switch-on sequence to SwitchedOn.\n");
   printf("  --cia402-enable      Run CiA402 enable-operation sequence.\n");
   printf("  --cia402-disable     Run CiA402 disable-voltage sequence.\n");
   printf("  --sequence-timeout-ms N  Sequence timeout. Default: %d\n",
          DEFAULT_SEQUENCE_TIMEOUT_MS);
   printf("  --help               Show this help.\n");
}

static int parse_options(int argc, char **argv, MonitorOptions *options)
{
   int i;

   memset(options, 0, sizeof(*options));
   options->vendor_id = LTS_VENDOR_ID;
   options->product_code = LTS_PRODUCT_CODE;
   options->period_us = DEFAULT_PERIOD_US;
   options->print_every = DEFAULT_PRINT_EVERY;
   options->priority = 80;
   options->configure_pdos = 1;
   options->sequence = CIA402_SEQ_NONE;
   options->sequence_timeout_ms = DEFAULT_SEQUENCE_TIMEOUT_MS;

   for (i = 1; i < argc; ++i)
   {
      if (strcmp(argv[i], "--help") == 0)
      {
         usage(argv[0]);
         return 1;
      }
      else if (strcmp(argv[i], "--period-us") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->period_us) != 0 ||
             options->period_us <= 0)
         {
            fprintf(stderr, "Invalid --period-us value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--print-every") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->print_every) != 0 ||
             options->print_every <= 0)
         {
            fprintf(stderr, "Invalid --print-every value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->max_cycles) != 0 ||
             options->max_cycles < 0)
         {
            fprintf(stderr, "Invalid --cycles value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--position") == 0 && i + 1 < argc)
      {
         uint32_t value;
         if (parse_u32(argv[++i], &value) != 0 || value > UINT16_MAX)
         {
            fprintf(stderr, "Invalid --position value.\n");
            return -1;
         }
         options->position = value;
      }
      else if (strcmp(argv[i], "--vendor") == 0 && i + 1 < argc)
      {
         if (parse_u32(argv[++i], &options->vendor_id) != 0)
         {
            fprintf(stderr, "Invalid --vendor value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--product") == 0 && i + 1 < argc)
      {
         if (parse_u32(argv[++i], &options->product_code) != 0)
         {
            fprintf(stderr, "Invalid --product value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--controlword") == 0 && i + 1 < argc)
      {
         uint32_t value;
         if (parse_u32(argv[++i], &value) != 0 || value > UINT16_MAX)
         {
            fprintf(stderr, "Invalid --controlword value.\n");
            return -1;
         }
         options->controlword = (uint16_t)value;
      }
      else if (strcmp(argv[i], "--target-position") == 0 && i + 1 < argc)
      {
         if (parse_i32(argv[++i], &options->target_position) != 0)
         {
            fprintf(stderr, "Invalid --target-position value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--target-velocity") == 0 && i + 1 < argc)
      {
         if (parse_i32(argv[++i], &options->target_velocity) != 0)
         {
            fprintf(stderr, "Invalid --target-velocity value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
      {
         int value;
         if (parse_int(argv[++i], &value) != 0 ||
             value < INT8_MIN || value > INT8_MAX)
         {
            fprintf(stderr, "Invalid --mode value.\n");
            return -1;
         }
         options->mode = (int8_t)value;
      }
      else if (strcmp(argv[i], "--priority") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->priority) != 0 ||
             options->priority < 1 || options->priority > 99)
         {
            fprintf(stderr, "Invalid --priority value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--use-sii-pdos") == 0)
      {
         options->configure_pdos = 0;
      }
      else if (strcmp(argv[i], "--fault-reset") == 0)
      {
         options->sequence = CIA402_SEQ_FAULT_RESET;
      }
      else if (strcmp(argv[i], "--cia402-shutdown") == 0)
      {
         options->sequence = CIA402_SEQ_SHUTDOWN;
      }
      else if (strcmp(argv[i], "--cia402-switch-on") == 0)
      {
         options->sequence = CIA402_SEQ_SWITCH_ON;
      }
      else if (strcmp(argv[i], "--cia402-enable") == 0)
      {
         options->sequence = CIA402_SEQ_ENABLE;
      }
      else if (strcmp(argv[i], "--cia402-disable") == 0)
      {
         options->sequence = CIA402_SEQ_DISABLE;
      }
      else if (strcmp(argv[i], "--sequence-timeout-ms") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->sequence_timeout_ms) != 0 ||
             options->sequence_timeout_ms <= 0)
         {
            fprintf(stderr, "Invalid --sequence-timeout-ms value.\n");
            return -1;
         }
      }
      else
      {
         fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
         usage(argv[0]);
         return -1;
      }
   }

   return 0;
}

static void add_ns(struct timespec *time, int64_t ns)
{
   time->tv_nsec += ns;
   while (time->tv_nsec >= 1000000000L)
   {
      time->tv_nsec -= 1000000000L;
      ++time->tv_sec;
   }
}

static int64_t timespec_to_ns(const struct timespec *time)
{
   return ((int64_t)time->tv_sec * 1000000000LL) + (int64_t)time->tv_nsec;
}

static int64_t ns_to_us(int64_t ns)
{
   return ns / 1000LL;
}

static int64_t abs_i64(int64_t value)
{
   return value < 0 ? -value : value;
}

static int setup_realtime(int priority)
{
   struct sched_param param;

   if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
   {
      perror("mlockall");
   }

   memset(&param, 0, sizeof(param));
   param.sched_priority = priority;
   if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
   {
      perror("sched_setscheduler");
      fprintf(stderr, "Continuing without SCHED_FIFO. Run with sudo for RT priority.\n");
      return -1;
   }

   return 0;
}

static const char *wc_state_text(ec_wc_state_t state)
{
   switch (state)
   {
   case EC_WC_ZERO:
      return "ZERO";
   case EC_WC_INCOMPLETE:
      return "INCOMPLETE";
   case EC_WC_COMPLETE:
      return "COMPLETE";
   default:
      return "UNKNOWN";
   }
}

static Cia402State cia402_state_from_status(uint16_t statusword)
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

static const char *cia402_status_text(Cia402State state)
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

static const char *cia402_mode_text(int8_t mode)
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

static const char *cia402_sequence_text(Cia402Sequence sequence)
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

static uint16_t cia402_sequence_controlword(Cia402Sequence sequence,
                                            Cia402State state,
                                            uint16_t manual_controlword,
                                            int *target_reached)
{
   if (target_reached != NULL)
   {
      *target_reached = 0;
   }

   switch (sequence)
   {
   case CIA402_SEQ_NONE:
      if (target_reached != NULL)
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
      if (target_reached != NULL)
      {
         *target_reached = 1;
      }
      return 0x0000U;

   case CIA402_SEQ_SHUTDOWN:
      if (state == CIA402_STATE_READY_TO_SWITCH_ON)
      {
         if (target_reached != NULL)
         {
            *target_reached = 1;
         }
      }
      return 0x0006U;

   case CIA402_SEQ_SWITCH_ON:
      if (state == CIA402_STATE_SWITCHED_ON ||
          state == CIA402_STATE_OPERATION_ENABLED)
      {
         if (target_reached != NULL)
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
         if (target_reached != NULL)
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
         if (target_reached != NULL)
         {
            *target_reached = 1;
         }
      }
      return 0x0000U;

   default:
      return manual_controlword;
   }
}

static void print_startup(const MonitorOptions *options, const ec_slave_info_t *info)
{
   printf("IgH LTS cyclic PDO monitor\n");
   printf("  Xenomai POSIX: %s\n",
#ifdef ECAT_USE_XENOMAI_POSIX
          "enabled"
#else
          "not linked"
#endif
   );
   printf("  period: %d us\n", options->period_us);
   printf("  slave position: %u\n", options->position);
   printf("  vendor/product: 0x%08x / 0x%08x\n",
          options->vendor_id, options->product_code);
   if (info != NULL)
   {
      printf("  detected slave: %s, rev=0x%08x, state=0x%02x\n",
             info->name,
             info->revision_number,
             info->al_state);
   }
   printf("  safe outputs: cw=0x%04x, target_pos=%d, target_vel=%d, mode=%d\n",
          options->controlword,
          options->target_position,
          options->target_velocity,
          options->mode);
   printf("  PDO config: %s\n",
          options->configure_pdos ? "write SM2/SM3 map" : "use SII/default map");
   printf("  CiA402 sequence: %s, timeout=%d ms\n",
          cia402_sequence_text(options->sequence),
          options->sequence_timeout_ms);
}

int main(int argc, char **argv)
{
   MonitorOptions options;
   LtsOffsets off;
   unsigned int bit_positions[8];
   ec_pdo_entry_reg_t regs[9];
   ec_master_t *master = NULL;
   ec_domain_t *domain = NULL;
   ec_slave_config_t *sc = NULL;
   ec_slave_info_t slave_info;
   ec_domain_state_t domain_state;
   ec_master_state_t master_state;
   ec_slave_config_state_t sc_state;
   uint8_t *domain_pd = NULL;
   struct timespec wakeup_time;
   struct timespec loop_time;
   struct timespec previous_loop_time;
   int64_t period_ns;
   int64_t min_loop_us = LLONG_MAX;
   int64_t max_loop_us = 0;
   int64_t sum_loop_us = 0;
   int64_t max_period_error_us = 0;
   int timing_samples = 0;
   unsigned int expected_wkc = 0;
   int wkc_errors = 0;
   int state_errors = 0;
   int op_seen = 0;
   int sequence_done = 1;
   int sequence_failed = 0;
   int sequence_done_cycle = -1;
   uint16_t command_controlword = 0;
   int cycle = 0;
   int parse_result;
   int result = 1;

   parse_result = parse_options(argc, argv, &options);
   if (parse_result > 0)
   {
      return 0;
   }
   if (parse_result < 0)
   {
      return 2;
   }

   sequence_done = options.sequence == CIA402_SEQ_NONE ? 1 : 0;
   command_controlword = options.controlword;

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);

   memset(&off, 0, sizeof(off));
   memset(bit_positions, 0, sizeof(bit_positions));
   memset(regs, 0, sizeof(regs));
   regs[0] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6040, 0x00, &off.controlword, &bit_positions[0]};
   regs[1] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x607a, 0x00, &off.target_position, &bit_positions[1]};
   regs[2] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x60ff, 0x00, &off.target_velocity, &bit_positions[2]};
   regs[3] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6060, 0x00, &off.mode_of_operation, &bit_positions[3]};
   regs[4] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6041, 0x00, &off.statusword, &bit_positions[4]};
   regs[5] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6064, 0x00, &off.actual_position, &bit_positions[5]};
   regs[6] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x606c, 0x00, &off.actual_velocity, &bit_positions[6]};
   regs[7] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6061, 0x00, &off.mode_display, &bit_positions[7]};
   regs[8] = (ec_pdo_entry_reg_t){0, 0, 0, 0, 0, 0, NULL, NULL};

   master = ecrt_request_master(options.master_index);
   if (master == NULL)
   {
      fprintf(stderr, "Failed to request EtherCAT master %u.\n", options.master_index);
      fprintf(stderr, "Check: sudo systemctl status ethercat, sudo ethercat master\n");
      goto out;
   }

   if (ecrt_master_get_slave(master, options.position, &slave_info) == 0)
   {
      print_startup(&options, &slave_info);
   }
   else
   {
      print_startup(&options, NULL);
   }

   domain = ecrt_master_create_domain(master);
   if (domain == NULL)
   {
      fprintf(stderr, "Failed to create process data domain.\n");
      goto out;
   }

   sc = ecrt_master_slave_config(master,
                                 (uint16_t)options.alias,
                                 (uint16_t)options.position,
                                 options.vendor_id,
                                 options.product_code);
   if (sc == NULL)
   {
      fprintf(stderr, "Failed to get slave configuration.\n");
      goto out;
   }

   if (options.configure_pdos &&
       ecrt_slave_config_pdos(sc, EC_END, G_lts_syncs) != 0)
   {
      fprintf(stderr, "Failed to configure LTS PDOs.\n");
      goto out;
   }

   if (ecrt_domain_reg_pdo_entry_list(domain, regs) != 0)
   {
      fprintf(stderr, "Failed to register PDO entries.\n");
      goto out;
   }

   printf("  PDO offsets: cw=%u tp=%u tv=%u mode=%u sw=%u pos=%u vel=%u md=%u\n",
          off.controlword,
          off.target_position,
          off.target_velocity,
          off.mode_of_operation,
          off.statusword,
          off.actual_position,
          off.actual_velocity,
          off.mode_display);

   if (ecrt_master_activate(master) != 0)
   {
      fprintf(stderr, "Failed to activate master.\n");
      goto out;
   }

   domain_pd = ecrt_domain_data(domain);
   if (domain_pd == NULL)
   {
      fprintf(stderr, "Failed to get domain process data pointer.\n");
      goto out;
   }

   EC_WRITE_U16(domain_pd + off.controlword, options.controlword);
   EC_WRITE_S32(domain_pd + off.target_position, options.target_position);
   EC_WRITE_S32(domain_pd + off.target_velocity, options.target_velocity);
   EC_WRITE_S8(domain_pd + off.mode_of_operation, options.mode);
   ecrt_domain_queue(domain);
   ecrt_master_send(master);

   (void)setup_realtime(options.priority);

   period_ns = (int64_t)options.period_us * 1000LL;
   clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
   previous_loop_time = wakeup_time;
   add_ns(&wakeup_time, period_ns);

   while (!G_stop)
   {
      uint16_t statusword;
      int32_t actual_position;
      int32_t actual_velocity;
      int8_t mode_display;
      Cia402State drive_state;
      int sequence_target_reached = 0;

      if (options.max_cycles > 0 && cycle >= options.max_cycles)
      {
         break;
      }

      clock_gettime(CLOCK_MONOTONIC, &loop_time);
      if (cycle > 0)
      {
         int64_t elapsed_us =
            ns_to_us(timespec_to_ns(&loop_time) -
                     timespec_to_ns(&previous_loop_time));
         int64_t period_error_us = abs_i64(elapsed_us - options.period_us);

         if (elapsed_us < min_loop_us)
         {
            min_loop_us = elapsed_us;
         }
         if (elapsed_us > max_loop_us)
         {
            max_loop_us = elapsed_us;
         }
         if (period_error_us > max_period_error_us)
         {
            max_period_error_us = period_error_us;
         }
         sum_loop_us += elapsed_us;
         ++timing_samples;
      }
      previous_loop_time = loop_time;

      ecrt_master_receive(master);
      ecrt_domain_process(domain);

      statusword = EC_READ_U16(domain_pd + off.statusword);
      actual_position = EC_READ_S32(domain_pd + off.actual_position);
      actual_velocity = EC_READ_S32(domain_pd + off.actual_velocity);
      mode_display = EC_READ_S8(domain_pd + off.mode_display);
      drive_state = cia402_state_from_status(statusword);

      memset(&domain_state, 0, sizeof(domain_state));
      memset(&master_state, 0, sizeof(master_state));
      memset(&sc_state, 0, sizeof(sc_state));
      (void)ecrt_domain_state(domain, &domain_state);
      (void)ecrt_master_state(master, &master_state);
      (void)ecrt_slave_config_state(sc, &sc_state);

      if (domain_state.wc_state == EC_WC_COMPLETE &&
          domain_state.working_counter > 0U &&
          expected_wkc == 0U)
      {
         expected_wkc = domain_state.working_counter;
      }
      else if (expected_wkc > 0U &&
               (domain_state.wc_state != EC_WC_COMPLETE ||
                domain_state.working_counter != expected_wkc))
      {
         ++wkc_errors;
      }

      if (sc_state.operational)
      {
         op_seen = 1;
      }
      else if (op_seen)
      {
         ++state_errors;
      }

      command_controlword =
         cia402_sequence_controlword(options.sequence,
                                     drive_state,
                                     options.controlword,
                                     &sequence_target_reached);

      if (!sequence_done && sequence_target_reached)
      {
         sequence_done = 1;
         sequence_done_cycle = cycle;
      }
      if (!sequence_done && !sequence_failed &&
          (((int64_t)cycle * options.period_us) / 1000LL) >=
             options.sequence_timeout_ms)
      {
         sequence_failed = 1;
      }

      EC_WRITE_U16(domain_pd + off.controlword, command_controlword);
      EC_WRITE_S32(domain_pd + off.target_position, options.target_position);
      EC_WRITE_S32(domain_pd + off.target_velocity, options.target_velocity);
      EC_WRITE_S8(domain_pd + off.mode_of_operation, options.mode);

      if (cycle % options.print_every == 0)
      {
         int64_t avg_loop_us =
            timing_samples > 0 ? (sum_loop_us / timing_samples) : 0;
         int64_t print_min_loop_us =
            timing_samples > 0 ? min_loop_us : 0;

         printf("cyc=%d wkc=%u/%s master{slaves=%u al=0x%x link=%u} "
                "slave{online=%u op=%u al=0x%x} "
                "sw=0x%04x/%s cw=0x%04x pos=%d vel=%d mode=%d/%s "
                "seq{%s done=%d fail=%d done_cyc=%d} "
                "rt{min/avg/max=%lld/%lld/%lldus jitter_max=%lldus} "
                "err{wkc=%d state=%d}\n",
                cycle,
                domain_state.working_counter,
                wc_state_text(domain_state.wc_state),
                master_state.slaves_responding,
                master_state.al_states,
                master_state.link_up,
                sc_state.online,
                sc_state.operational,
                sc_state.al_state,
                statusword,
                cia402_status_text(drive_state),
                command_controlword,
                actual_position,
                actual_velocity,
                mode_display,
                cia402_mode_text(mode_display),
                cia402_sequence_text(options.sequence),
                sequence_done,
                sequence_failed,
                sequence_done_cycle,
                (long long)print_min_loop_us,
                (long long)avg_loop_us,
                (long long)max_loop_us,
                (long long)max_period_error_us,
                wkc_errors,
                state_errors);
      }

      ecrt_domain_queue(domain);
      ecrt_master_send(master);

      ++cycle;
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);
      add_ns(&wakeup_time, period_ns);
   }

   result = 0;

out:
   if (result == 0)
   {
      int64_t avg_loop_us =
         timing_samples > 0 ? (sum_loop_us / timing_samples) : 0;
      int64_t print_min_loop_us =
         timing_samples > 0 ? min_loop_us : 0;

      printf("summary: cycles=%d expected_wkc=%u wkc_errors=%d state_errors=%d "
             "sequence{%s done=%d fail=%d done_cyc=%d} "
             "cycle_us{min=%lld avg=%lld max=%lld jitter_max=%lld}\n",
             cycle,
             expected_wkc,
             wkc_errors,
             state_errors,
             cia402_sequence_text(options.sequence),
             sequence_done,
             sequence_failed,
             sequence_done_cycle,
             (long long)print_min_loop_us,
             (long long)avg_loop_us,
             (long long)max_loop_us,
             (long long)max_period_error_us);
   }

   if (master != NULL)
   {
      ecrt_release_master(master);
   }

   return result;
}
