#include <ecrt.h>

#include <errno.h>
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
   {0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE},
   {1, EC_DIR_INPUT, 0, NULL, EC_WD_DISABLE},
   {2, EC_DIR_OUTPUT, 1, &G_lts_pdos[0], EC_WD_ENABLE},
   {3, EC_DIR_INPUT, 1, &G_lts_pdos[1], EC_WD_DISABLE},
   {0xff}
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
}

int main(int argc, char **argv)
{
   MonitorOptions options;
   LtsOffsets off;
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
   int64_t period_ns;
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

   signal(SIGINT, on_signal);
   signal(SIGTERM, on_signal);

   memset(&off, 0, sizeof(off));
   memset(regs, 0, sizeof(regs));
   regs[0] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6040, 0x00, &off.controlword};
   regs[1] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x607a, 0x00, &off.target_position};
   regs[2] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x60ff, 0x00, &off.target_velocity};
   regs[3] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6060, 0x00, &off.mode_of_operation};
   regs[4] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6041, 0x00, &off.statusword};
   regs[5] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6064, 0x00, &off.actual_position};
   regs[6] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x606c, 0x00, &off.actual_velocity};
   regs[7] = (ec_pdo_entry_reg_t){options.alias, options.position, options.vendor_id, options.product_code, 0x6061, 0x00, &off.mode_display};

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

   if (ecrt_slave_config_pdos(sc, EC_END, G_lts_syncs) != 0)
   {
      fprintf(stderr, "Failed to configure LTS PDOs.\n");
      goto out;
   }

   if (ecrt_domain_reg_pdo_entry_list(domain, regs) != 0)
   {
      fprintf(stderr, "Failed to register PDO entries.\n");
      goto out;
   }

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

   (void)setup_realtime(options.priority);

   period_ns = (int64_t)options.period_us * 1000LL;
   clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
   add_ns(&wakeup_time, period_ns);

   while (!G_stop)
   {
      uint16_t statusword;
      int32_t actual_position;
      int32_t actual_velocity;
      int8_t mode_display;

      if (options.max_cycles > 0 && cycle >= options.max_cycles)
      {
         break;
      }

      ecrt_master_receive(master);
      ecrt_domain_process(domain);

      EC_WRITE_U16(domain_pd + off.controlword, options.controlword);
      EC_WRITE_S32(domain_pd + off.target_position, options.target_position);
      EC_WRITE_S32(domain_pd + off.target_velocity, options.target_velocity);
      EC_WRITE_S8(domain_pd + off.mode_of_operation, options.mode);

      statusword = EC_READ_U16(domain_pd + off.statusword);
      actual_position = EC_READ_S32(domain_pd + off.actual_position);
      actual_velocity = EC_READ_S32(domain_pd + off.actual_velocity);
      mode_display = EC_READ_S8(domain_pd + off.mode_display);

      if (cycle % options.print_every == 0)
      {
         memset(&domain_state, 0, sizeof(domain_state));
         memset(&master_state, 0, sizeof(master_state));
         memset(&sc_state, 0, sizeof(sc_state));
         (void)ecrt_domain_state(domain, &domain_state);
         (void)ecrt_master_state(master, &master_state);
         (void)ecrt_slave_config_state(sc, &sc_state);

         printf("cyc=%d wkc=%u/%s master{slaves=%u al=0x%x link=%u} "
                "slave{online=%u op=%u al=0x%x} sw=0x%04x pos=%d vel=%d mode=%d\n",
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
                actual_position,
                actual_velocity,
                mode_display);
      }

      ecrt_domain_queue(domain);
      ecrt_master_send(master);

      ++cycle;
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);
      add_ns(&wakeup_time, period_ns);
   }

   result = 0;

out:
   if (master != NULL)
   {
      ecrt_release_master(master);
   }

   return result;
}
