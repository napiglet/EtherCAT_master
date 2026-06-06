#include <ecrt.h>

#include "ecat_protocol.h"
#include "motion_cia402.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LTS_VENDOR_ID 0x0000205eu
#define LTS_PRODUCT_CODE 0x90000300u
#define DEFAULT_PERIOD_US 1000
#define DEFAULT_PRIORITY 80
#define DEFAULT_SEQUENCE_TIMEOUT_MS 5000
#define DEFAULT_CLIENT_TIMEOUT_MS 1000
#define LTS_PDO_BYTES 14

typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close

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

typedef struct BackendOptions
{
   int port;
   unsigned int master_index;
   unsigned int alias;
   unsigned int position;
   uint32_t vendor_id;
   uint32_t product_code;
   int period_us;
   int priority;
   int configure_pdos;
   int sequence_timeout_ms;
   int client_timeout_ms;
} BackendOptions;

typedef struct BackendCommand
{
   uint16_t controlword;
   int32_t target_position;
   int32_t target_velocity;
   int8_t mode;
   Cia402Sequence sequence;
   Cia402MotionCommand motion;
   uint32_t generation;
   uint32_t reset_generation;
} BackendCommand;

typedef struct BackendRuntime
{
   BackendOptions options;
   pthread_mutex_t lock;
   pthread_t thread;
   int thread_started;
   int stop_requested;
   int initialized;
   int fatal_error;
   int safety_stop_active;
   int safety_stop_count;
   int64_t last_client_message_ns;
   uint32_t response_sequence;
   BackendCommand command;
   ECAT_NetStatus status;
} BackendRuntime;

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

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
   if (dst_size == 0)
   {
      return;
   }
   if (src == NULL)
   {
      src = "";
   }
   (void)snprintf(dst, dst_size, "%s", src);
}

static void sleep_ms(int ms)
{
   struct timespec ts;
   ts.tv_sec = ms / 1000;
   ts.tv_nsec = (long)(ms % 1000) * 1000000L;
   (void)nanosleep(&ts, NULL);
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

static int is_number_text(const char *text)
{
   const char *p = text;

   if (p == NULL || *p == '\0')
   {
      return 0;
   }
   while (*p != '\0')
   {
      if (*p < '0' || *p > '9')
      {
         return 0;
      }
      ++p;
   }
   return 1;
}

static void usage(const char *argv0)
{
   printf("Usage: %s [port] [options]\n", argv0);
   printf("\n");
   printf("IgH/Xenomai TCP backend server for Windows GUI/DLL.\n");
   printf("The first target is LTS_MotorDriver1x using the confirmed PDO map.\n");
   printf("\n");
   printf("Options:\n");
   printf("  --port N             TCP port. Default: %d\n", ECAT_NET_DEFAULT_PORT);
   printf("  --period-us N        Cycle period in us. Default: %d\n", DEFAULT_PERIOD_US);
   printf("  --position N         Slave ring position. Default: 0\n");
   printf("  --vendor HEX         Vendor ID. Default: 0x%08x\n", LTS_VENDOR_ID);
   printf("  --product HEX        Product code. Default: 0x%08x\n", LTS_PRODUCT_CODE);
   printf("  --priority N         SCHED_FIFO priority. Default: %d\n", DEFAULT_PRIORITY);
   printf("  --use-sii-pdos       Use slave/default SII PDO map.\n");
   printf("  --sequence-timeout-ms N  CiA402 sequence timeout. Default: %d\n",
          DEFAULT_SEQUENCE_TIMEOUT_MS);
   printf("  --client-timeout-ms N    Safety stop when no client message arrives. Default: %d\n",
          DEFAULT_CLIENT_TIMEOUT_MS);
   printf("                           Use 0 to disable client heartbeat timeout.\n");
   printf("  --help               Show this help.\n");
}

static void options_init(BackendOptions *options)
{
   memset(options, 0, sizeof(*options));
   options->port = ECAT_NET_DEFAULT_PORT;
   options->vendor_id = LTS_VENDOR_ID;
   options->product_code = LTS_PRODUCT_CODE;
   options->period_us = DEFAULT_PERIOD_US;
   options->priority = DEFAULT_PRIORITY;
   options->configure_pdos = 1;
   options->sequence_timeout_ms = DEFAULT_SEQUENCE_TIMEOUT_MS;
   options->client_timeout_ms = DEFAULT_CLIENT_TIMEOUT_MS;
}

static int parse_options(int argc, char **argv, BackendOptions *options)
{
   int i;

   options_init(options);

   for (i = 1; i < argc; ++i)
   {
      if (i == 1 && is_number_text(argv[i]))
      {
         if (parse_int(argv[i], &options->port) != 0 || options->port <= 0)
         {
            fprintf(stderr, "Invalid port value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--help") == 0)
      {
         usage(argv[0]);
         return 1;
      }
      else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->port) != 0 ||
             options->port <= 0)
         {
            fprintf(stderr, "Invalid --port value.\n");
            return -1;
         }
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
      else if (strcmp(argv[i], "--sequence-timeout-ms") == 0 &&
               i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->sequence_timeout_ms) != 0 ||
             options->sequence_timeout_ms <= 0)
         {
            fprintf(stderr, "Invalid --sequence-timeout-ms value.\n");
            return -1;
         }
      }
      else if (strcmp(argv[i], "--client-timeout-ms") == 0 &&
               i + 1 < argc)
      {
         if (parse_int(argv[++i], &options->client_timeout_ms) != 0 ||
             options->client_timeout_ms < 0)
         {
            fprintf(stderr, "Invalid --client-timeout-ms value.\n");
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

static int send_all(socket_t sock, const void *data, size_t size)
{
   const char *p = (const char *)data;
   size_t sent = 0;

   while (sent < size)
   {
      ssize_t n = send(sock, p + sent, size - sent, 0);
      if (n <= 0)
      {
         return -1;
      }
      sent += (size_t)n;
   }
   return 0;
}

static int recv_all(socket_t sock, void *data, size_t size)
{
   char *p = (char *)data;
   size_t received = 0;

   while (received < size)
   {
      ssize_t n = recv(sock, p + received, size - received, 0);
      if (n <= 0)
      {
         return -1;
      }
      received += (size_t)n;
   }
   return 0;
}

static int send_message(socket_t sock, uint16_t type, uint32_t sequence,
                        const void *payload, uint32_t payload_size)
{
   ECAT_NetHeader header;

   memset(&header, 0, sizeof(header));
   header.magic = ECAT_NET_MAGIC;
   header.version = ECAT_NET_VERSION;
   header.type = type;
   header.size = payload_size;
   header.sequence = sequence;

   if (send_all(sock, &header, sizeof(header)) != 0)
   {
      return -1;
   }
   if (payload_size > 0 && payload != NULL)
   {
      return send_all(sock, payload, payload_size);
   }
   return 0;
}

static int recv_message(socket_t sock, ECAT_NetHeader *header, void *payload,
                        uint32_t payload_capacity)
{
   if (recv_all(sock, header, sizeof(*header)) != 0)
   {
      return -1;
   }
   if (header->magic != ECAT_NET_MAGIC ||
       header->version != ECAT_NET_VERSION ||
       header->size > payload_capacity)
   {
      return -1;
   }
   if (header->size > 0)
   {
      return recv_all(sock, payload, header->size);
   }
   return 0;
}

static uint16_t read_le_u16(const uint8_t *data)
{
   return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int32_t read_le_i32(const uint8_t *data)
{
   uint32_t raw = (uint32_t)data[0] |
                  ((uint32_t)data[1] << 8) |
                  ((uint32_t)data[2] << 16) |
                  ((uint32_t)data[3] << 24);
   return (int32_t)raw;
}

static void write_le_u16(uint8_t *data, uint16_t value)
{
   data[0] = (uint8_t)(value & 0xffu);
   data[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void write_le_i32(uint8_t *data, int32_t value)
{
   uint32_t raw = (uint32_t)value;

   data[0] = (uint8_t)(raw & 0xffu);
   data[1] = (uint8_t)((raw >> 8) & 0xffu);
   data[2] = (uint8_t)((raw >> 16) & 0xffu);
   data[3] = (uint8_t)((raw >> 24) & 0xffu);
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

static int64_t monotonic_now_ns(void)
{
   struct timespec now;

   clock_gettime(CLOCK_MONOTONIC, &now);
   return timespec_to_ns(&now);
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

static void runtime_init(BackendRuntime *rt, const BackendOptions *options)
{
   memset(rt, 0, sizeof(*rt));
   rt->options = *options;
   pthread_mutex_init(&rt->lock, NULL);
   cia402_motion_init(&rt->command.motion);
   rt->last_client_message_ns = monotonic_now_ns();
   rt->status.runtime.period_us = options->period_us;
   safe_copy(rt->status.runtime.state_text,
             sizeof(rt->status.runtime.state_text),
             "IgH backend starting");
   safe_copy(rt->status.runtime.crc_status,
             sizeof(rt->status.runtime.crc_status),
             "Ethernet FCS/CRC is handled by the NIC and IgH master.");
}

static void runtime_destroy(BackendRuntime *rt)
{
   pthread_mutex_destroy(&rt->lock);
}

static void runtime_set_fatal(BackendRuntime *rt, const char *message)
{
   pthread_mutex_lock(&rt->lock);
   rt->fatal_error = 1;
   rt->initialized = 0;
   rt->status.runtime.connected = 0;
   rt->status.runtime.operational = 0;
   safe_copy(rt->status.runtime.state_text,
             sizeof(rt->status.runtime.state_text),
             "IgH backend error");
   safe_copy(rt->status.runtime.last_error,
             sizeof(rt->status.runtime.last_error),
             message);
   pthread_mutex_unlock(&rt->lock);
}

static int runtime_should_stop(BackendRuntime *rt)
{
   int stop;

   pthread_mutex_lock(&rt->lock);
   stop = rt->stop_requested || rt->fatal_error || G_stop;
   pthread_mutex_unlock(&rt->lock);
   return stop;
}

static void runtime_request_stop(BackendRuntime *rt)
{
   pthread_mutex_lock(&rt->lock);
   rt->stop_requested = 1;
   pthread_mutex_unlock(&rt->lock);
}

static void runtime_get_status(BackendRuntime *rt, ECAT_NetStatus *status)
{
   pthread_mutex_lock(&rt->lock);
   *status = rt->status;
   pthread_mutex_unlock(&rt->lock);
}

static void runtime_reset_statistics_locked(BackendRuntime *rt)
{
   rt->command.reset_generation++;
   rt->safety_stop_count = 0;
}

static void backend_command_safe_stop(BackendCommand *command)
{
   command->controlword = 0;
   command->target_velocity = 0;
   command->sequence = CIA402_SEQ_NONE;
   cia402_motion_init(&command->motion);
}

static void runtime_mark_client_message(BackendRuntime *rt)
{
   pthread_mutex_lock(&rt->lock);
   rt->last_client_message_ns = monotonic_now_ns();
   rt->safety_stop_active = 0;
   pthread_mutex_unlock(&rt->lock);
}

static void runtime_apply_safety_stop_locked(BackendRuntime *rt,
                                             const char *reason)
{
   backend_command_safe_stop(&rt->command);
   rt->command.generation++;
   rt->safety_stop_active = 1;
   rt->safety_stop_count++;
   safe_copy(rt->status.runtime.last_error,
             sizeof(rt->status.runtime.last_error),
             reason);
}

static void runtime_apply_command(BackendRuntime *rt,
                                  const ECAT_NetCommand *net_command)
{
   BackendCommand *command;

   if (net_command == NULL)
   {
      return;
   }

   pthread_mutex_lock(&rt->lock);
   command = &rt->command;

   switch ((ECAT_NetCommandType)net_command->command)
   {
   case ECAT_NET_CMD_NONE:
   case ECAT_NET_CMD_START:
      break;

   case ECAT_NET_CMD_STOP:
      backend_command_safe_stop(command);
      command->generation++;
      break;

   case ECAT_NET_CMD_RESET_STATISTICS:
      runtime_reset_statistics_locked(rt);
      break;

   case ECAT_NET_CMD_SERVO_FAULT_RESET:
      command->sequence = CIA402_SEQ_FAULT_RESET;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_ENABLE:
      command->sequence = CIA402_SEQ_ENABLE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_DISABLE:
      command->sequence = CIA402_SEQ_DISABLE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_SET_MODE:
      command->mode = (int8_t)net_command->mode;
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_MOVE_ABS:
   case ECAT_NET_CMD_LMS_MOVE_ABS:
      command->target_position = net_command->target_position;
      command->sequence = CIA402_SEQ_ENABLE;
      cia402_motion_init(&command->motion);
      command->motion.type = CIA402_MOTION_PROFILE_POSITION;
      command->motion.mode = CIA402_MODE_PROFILE_POSITION;
      command->motion.target_position = net_command->target_position;
      command->motion.profile_velocity = (int32_t)net_command->velocity;
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_JOG:
   case ECAT_NET_CMD_LMS_MOVE_VEL:
      command->target_velocity = net_command->target_velocity;
      command->sequence = CIA402_SEQ_ENABLE;
      cia402_motion_init(&command->motion);
      command->motion.type = CIA402_MOTION_JOG_VELOCITY;
      command->motion.mode = CIA402_MODE_CSV;
      command->motion.target_velocity = net_command->target_velocity;
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_HOME:
      command->sequence = CIA402_SEQ_ENABLE;
      cia402_motion_init(&command->motion);
      command->motion.type = CIA402_MOTION_HOME;
      command->motion.mode = CIA402_MODE_HOMING;
      command->motion.target_velocity = (int32_t)net_command->search_speed;
      command->generation++;
      break;

   case ECAT_NET_CMD_SERVO_STOP:
   case ECAT_NET_CMD_LMS_STOP:
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->motion.type = CIA402_MOTION_SERVO_STOP;
      command->generation++;
      break;

   default:
      safe_copy(rt->status.runtime.last_error,
                sizeof(rt->status.runtime.last_error),
                "Unsupported backend command.");
      break;
   }

   pthread_mutex_unlock(&rt->lock);
}

static int runtime_read_cached_sdo(BackendRuntime *rt,
                                   const ECAT_NetSdoRequest *request,
                                   ECAT_NetSdoReply *reply)
{
   ECAT_NetStatus status;
   const ECAT_NetSlaveStatus *slave;
   const uint8_t *data = NULL;
   uint8_t local[4];
   int size = 0;

   memset(reply, 0, sizeof(*reply));
   runtime_get_status(rt, &status);
   slave = &status.slaves[0];

   switch (request->index)
   {
   case 0x6040:
      write_le_u16(local, slave->controlword);
      data = local;
      size = 2;
      break;
   case 0x6041:
      write_le_u16(local, slave->statusword);
      data = local;
      size = 2;
      break;
   case 0x6060:
      local[0] = (uint8_t)slave->outputs[10];
      data = local;
      size = 1;
      break;
   case 0x6061:
      local[0] = (uint8_t)slave->mode_display;
      data = local;
      size = 1;
      break;
   case 0x6064:
      write_le_i32(local, slave->actual_position);
      data = local;
      size = 4;
      break;
   case 0x606c:
      write_le_i32(local, slave->actual_velocity);
      data = local;
      size = 4;
      break;
   case 0x607a:
      write_le_i32(local, read_le_i32(&slave->outputs[2]));
      data = local;
      size = 4;
      break;
   case 0x60ff:
      write_le_i32(local, read_le_i32(&slave->outputs[6]));
      data = local;
      size = 4;
      break;
   case 0x603f:
      write_le_u16(local, 0);
      data = local;
      size = 2;
      break;
   default:
      reply->result = -1;
      return -1;
   }

   if (request->size > 0 && size > request->size)
   {
      size = request->size;
   }
   memcpy(reply->data, data, (size_t)size);
   reply->size = size;
   reply->result = 0;
   return 0;
}

static int runtime_write_cached_sdo(BackendRuntime *rt,
                                    const ECAT_NetSdoRequest *request,
                                    ECAT_NetSdoReply *reply)
{
   BackendCommand *command;

   memset(reply, 0, sizeof(*reply));
   if (request->size <= 0)
   {
      reply->result = -1;
      return -1;
   }

   pthread_mutex_lock(&rt->lock);
   command = &rt->command;

   switch (request->index)
   {
   case 0x6040:
      if (request->size < 2)
      {
         reply->result = -1;
         break;
      }
      command->controlword = read_le_u16(request->data);
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case 0x6060:
      command->mode = (int8_t)request->data[0];
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case 0x607a:
      if (request->size < 4)
      {
         reply->result = -1;
         break;
      }
      command->target_position = read_le_i32(request->data);
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   case 0x60ff:
      if (request->size < 4)
      {
         reply->result = -1;
         break;
      }
      command->target_velocity = read_le_i32(request->data);
      command->sequence = CIA402_SEQ_NONE;
      cia402_motion_init(&command->motion);
      command->generation++;
      break;

   default:
      reply->result = -1;
      break;
   }

   pthread_mutex_unlock(&rt->lock);
   return reply->result == 0 ? 0 : -1;
}

static void build_pdo_copies(ECAT_NetSlaveStatus *slave,
                             const Cia402PdoOutput *output,
                             uint16_t statusword,
                             int32_t actual_position,
                             int32_t actual_velocity,
                             int8_t mode_display)
{
   memset(slave->outputs, 0, sizeof(slave->outputs));
   memset(slave->inputs, 0, sizeof(slave->inputs));

   write_le_u16(&slave->outputs[0], output->controlword);
   write_le_i32(&slave->outputs[2], output->target_position);
   write_le_i32(&slave->outputs[6], output->target_velocity);
   slave->outputs[10] = (uint8_t)output->mode;

   write_le_u16(&slave->inputs[0], statusword);
   write_le_i32(&slave->inputs[2], actual_position);
   write_le_i32(&slave->inputs[6], actual_velocity);
   slave->inputs[10] = (uint8_t)mode_display;

   slave->output_size = LTS_PDO_BYTES;
   slave->input_size = LTS_PDO_BYTES;
}

static void *rt_thread_main(void *arg)
{
   BackendRuntime *rt = (BackendRuntime *)arg;
   BackendOptions options = rt->options;
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
   char slave_name[ECAT_NET_MAX_NAME];
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
   int cycle = 0;
   uint32_t command_generation_seen = UINT32_MAX;
   uint32_t reset_generation_seen = 0;
   int sequence_done = 1;
   int sequence_failed = 0;
   int sequence_start_cycle = 0;
   int sequence_done_cycle = -1;
   int result = 1;

   safe_copy(slave_name, sizeof(slave_name), "LTS_MotorDriver1x");
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
      runtime_set_fatal(rt, "Failed to request EtherCAT master.");
      goto out;
   }

   if (ecrt_master_get_slave(master, options.position, &slave_info) == 0)
   {
      safe_copy(slave_name, sizeof(slave_name), slave_info.name);
   }
   else
   {
      memset(&slave_info, 0, sizeof(slave_info));
   }

   domain = ecrt_master_create_domain(master);
   if (domain == NULL)
   {
      runtime_set_fatal(rt, "Failed to create process data domain.");
      goto out;
   }

   sc = ecrt_master_slave_config(master,
                                 (uint16_t)options.alias,
                                 (uint16_t)options.position,
                                 options.vendor_id,
                                 options.product_code);
   if (sc == NULL)
   {
      runtime_set_fatal(rt, "Failed to get slave configuration.");
      goto out;
   }

   if (options.configure_pdos &&
       ecrt_slave_config_pdos(sc, EC_END, G_lts_syncs) != 0)
   {
      runtime_set_fatal(rt, "Failed to configure LTS PDOs.");
      goto out;
   }

   if (ecrt_domain_reg_pdo_entry_list(domain, regs) != 0)
   {
      runtime_set_fatal(rt, "Failed to register PDO entries.");
      goto out;
   }

   (void)ecrt_master_set_send_interval(master,
                                       (size_t)options.period_us * 1000U);

   if (ecrt_master_activate(master) != 0)
   {
      runtime_set_fatal(rt, "Failed to activate EtherCAT master.");
      goto out;
   }

   domain_pd = ecrt_domain_data(domain);
   if (domain_pd == NULL)
   {
      runtime_set_fatal(rt, "Failed to get domain process data pointer.");
      goto out;
   }

   EC_WRITE_U16(domain_pd + off.controlword, 0);
   EC_WRITE_S32(domain_pd + off.target_position, 0);
   EC_WRITE_S32(domain_pd + off.target_velocity, 0);
   EC_WRITE_S8(domain_pd + off.mode_of_operation, 0);
   ecrt_domain_queue(domain);
   ecrt_master_send(master);

   (void)setup_realtime(options.priority);

   pthread_mutex_lock(&rt->lock);
   rt->initialized = 1;
   rt->fatal_error = 0;
   rt->status.runtime.connected = 1;
   rt->status.runtime.period_us = options.period_us;
   safe_copy(rt->status.runtime.state_text,
             sizeof(rt->status.runtime.state_text),
             "IgH backend running");
   safe_copy(rt->status.runtime.last_error,
             sizeof(rt->status.runtime.last_error),
             "");
   pthread_mutex_unlock(&rt->lock);

   printf("IgH backend server RT loop started: period=%dus slave=%s\n",
          options.period_us, slave_name);

   period_ns = (int64_t)options.period_us * 1000LL;
   clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
   previous_loop_time = wakeup_time;
   add_ns(&wakeup_time, period_ns);

   while (!runtime_should_stop(rt))
   {
      BackendCommand command;
      uint16_t statusword;
      int32_t actual_position;
      int32_t actual_velocity;
      int8_t mode_display;
      Cia402State drive_state;
      int sequence_target_reached = 0;
      int cycle_after_sequence = -1;
      Cia402PdoOutput pdo_output;
      int64_t avg_loop_us;
      int64_t print_min_loop_us;
      int safety_stop_active;

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

      pthread_mutex_lock(&rt->lock);
      if (options.client_timeout_ms > 0 &&
          rt->last_client_message_ns > 0 &&
          timespec_to_ns(&loop_time) - rt->last_client_message_ns >
             (int64_t)options.client_timeout_ms * 1000000LL)
      {
         if (!rt->safety_stop_active)
         {
            runtime_apply_safety_stop_locked(rt, "Client heartbeat timeout. Safe stop applied.");
         }
      }
      command = rt->command;
      safety_stop_active = rt->safety_stop_active;
      if (command.reset_generation != reset_generation_seen)
      {
         reset_generation_seen = command.reset_generation;
         min_loop_us = LLONG_MAX;
         max_loop_us = 0;
         sum_loop_us = 0;
         max_period_error_us = 0;
         timing_samples = 0;
         expected_wkc = 0;
         wkc_errors = 0;
         state_errors = 0;
         cycle = 0;
      }
      pthread_mutex_unlock(&rt->lock);

      if (command.generation != command_generation_seen)
      {
         command_generation_seen = command.generation;
         sequence_done = command.sequence == CIA402_SEQ_NONE ? 1 : 0;
         sequence_failed = 0;
         sequence_start_cycle = cycle;
         sequence_done_cycle = -1;
      }

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

      pdo_output.controlword =
         cia402_sequence_controlword(command.sequence,
                                     drive_state,
                                     command.controlword,
                                     &sequence_target_reached);
      pdo_output.target_position = command.target_position;
      pdo_output.target_velocity = command.target_velocity;
      pdo_output.mode = command.mode;

      if (!sequence_done && sequence_target_reached)
      {
         sequence_done = 1;
         sequence_done_cycle = cycle;
      }
      if (!sequence_done && !sequence_failed &&
          (((int64_t)(cycle - sequence_start_cycle) * options.period_us) /
           1000LL) >=
             options.sequence_timeout_ms)
      {
         sequence_failed = 1;
      }

      if (sequence_done_cycle >= 0)
      {
         cycle_after_sequence = cycle - sequence_done_cycle;
      }

      cia402_motion_apply(&command.motion,
                          sequence_done && !sequence_failed,
                          cycle_after_sequence,
                          &pdo_output);

      EC_WRITE_U16(domain_pd + off.controlword, pdo_output.controlword);
      EC_WRITE_S32(domain_pd + off.target_position, pdo_output.target_position);
      EC_WRITE_S32(domain_pd + off.target_velocity, pdo_output.target_velocity);
      EC_WRITE_S8(domain_pd + off.mode_of_operation, pdo_output.mode);

      avg_loop_us = timing_samples > 0 ? (sum_loop_us / timing_samples) : 0;
      print_min_loop_us = timing_samples > 0 ? min_loop_us : 0;

      pthread_mutex_lock(&rt->lock);
      memset(&rt->status, 0, sizeof(rt->status));
      rt->status.runtime.connected = 1;
      rt->status.runtime.operational = sc_state.operational ? 1 : 0;
      rt->status.runtime.slave_count = 1;
      rt->status.runtime.expected_wkc = (int32_t)expected_wkc;
      rt->status.runtime.last_wkc = (int32_t)domain_state.working_counter;
      rt->status.runtime.total_cycles = cycle;
      rt->status.runtime.wkc_errors = wkc_errors;
      rt->status.runtime.state_errors = state_errors;
      rt->status.runtime.cycle_us = options.period_us;
      rt->status.runtime.min_cycle_us = (int32_t)print_min_loop_us;
      rt->status.runtime.max_cycle_us = (int32_t)max_loop_us;
      rt->status.runtime.avg_cycle_us = (double)avg_loop_us;
      rt->status.runtime.period_us = options.period_us;
      (void)snprintf(rt->status.runtime.state_text,
                     sizeof(rt->status.runtime.state_text),
                     "IgH %s, slave %s, CiA402 %s, mode %s, seq %s, motion %s",
                     wc_state_text(domain_state.wc_state),
                     sc_state.operational ? "OP" : "not OP",
                     cia402_status_text(drive_state),
                     cia402_mode_text(mode_display),
                     cia402_sequence_text(command.sequence),
                     safety_stop_active ? "SafetyStop" :
                        cia402_motion_text(command.motion.type));
      if (safety_stop_active)
      {
         safe_copy(rt->status.runtime.last_error,
                   sizeof(rt->status.runtime.last_error),
                   "Client heartbeat timeout. Safe stop applied.");
      }
      else if (sequence_failed)
      {
         safe_copy(rt->status.runtime.last_error,
                   sizeof(rt->status.runtime.last_error),
                   "CiA402 sequence timeout.");
      }
      else
      {
         safe_copy(rt->status.runtime.last_error,
                   sizeof(rt->status.runtime.last_error),
                   "");
      }
      (void)snprintf(rt->status.runtime.crc_status,
                     sizeof(rt->status.runtime.crc_status),
                     "WKC %u/%s, wkc_errors=%d, state_errors=%d, jitter_max=%lldus, safety_stops=%d",
                     domain_state.working_counter,
                     wc_state_text(domain_state.wc_state),
                     wkc_errors,
                     state_errors,
                     (long long)max_period_error_us,
                     rt->safety_stop_count);

      {
         ECAT_NetSlaveStatus *slave = &rt->status.slaves[0];
         slave->index = 1;
         safe_copy(slave->name, sizeof(slave->name), slave_name);
         slave->state = sc_state.al_state;
         slave->al_status = sc_state.al_state;
         slave->vendor_id = options.vendor_id;
         slave->product_code = options.product_code;
         slave->revision = slave_info.revision_number;
         slave->output_bytes = LTS_PDO_BYTES;
         slave->input_bytes = LTS_PDO_BYTES;
         slave->has_dc = 0;
         slave->is_lost = sc_state.online ? 0 : 1;
         slave->statusword = statusword;
         slave->controlword = pdo_output.controlword;
         slave->mode_display = mode_display;
         slave->actual_position = actual_position;
         slave->actual_velocity = actual_velocity;
         build_pdo_copies(slave,
                          &pdo_output,
                          statusword,
                          actual_position,
                          actual_velocity,
                          mode_display);
      }
      pthread_mutex_unlock(&rt->lock);

      ecrt_domain_queue(domain);
      ecrt_master_send(master);

      ++cycle;
      (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wakeup_time, NULL);
      add_ns(&wakeup_time, period_ns);
   }

   result = 0;

out:
   if (domain_pd != NULL)
   {
      EC_WRITE_U16(domain_pd + off.controlword, 0);
      EC_WRITE_S32(domain_pd + off.target_position, 0);
      EC_WRITE_S32(domain_pd + off.target_velocity, 0);
      EC_WRITE_S8(domain_pd + off.mode_of_operation, 0);
      ecrt_domain_queue(domain);
      ecrt_master_send(master);
   }
   if (master != NULL)
   {
      ecrt_release_master(master);
   }

   pthread_mutex_lock(&rt->lock);
   rt->initialized = 0;
   rt->status.runtime.connected = 0;
   rt->status.runtime.operational = 0;
   if (result == 0)
   {
      safe_copy(rt->status.runtime.state_text,
                sizeof(rt->status.runtime.state_text),
                "IgH backend stopped");
   }
   pthread_mutex_unlock(&rt->lock);

   return NULL;
}

static int runtime_start(BackendRuntime *rt)
{
   int result;

   result = pthread_create(&rt->thread, NULL, rt_thread_main, rt);
   if (result != 0)
   {
      runtime_set_fatal(rt, "Failed to create RT thread.");
      return -1;
   }
   rt->thread_started = 1;
   return 0;
}

static void runtime_stop_and_join(BackendRuntime *rt)
{
   runtime_request_stop(rt);
   if (rt->thread_started)
   {
      pthread_join(rt->thread, NULL);
      rt->thread_started = 0;
   }
}

static void runtime_wait_first_status(BackendRuntime *rt)
{
   int i;

   for (i = 0; i < 500; ++i)
   {
      int done;

      pthread_mutex_lock(&rt->lock);
      done = rt->initialized || rt->fatal_error;
      pthread_mutex_unlock(&rt->lock);
      if (done)
      {
         return;
      }
      sleep_ms(10);
   }
}

static int send_runtime_status(socket_t client, BackendRuntime *rt,
                               uint32_t sequence)
{
   ECAT_NetStatus status;

   runtime_get_status(rt, &status);
   return send_message(client, ECAT_NET_MSG_STATUS, sequence,
                       &status, sizeof(status));
}

static int handle_client(socket_t client, const BackendOptions *base_options)
{
   BackendOptions options = *base_options;
   BackendRuntime rt;
   ECAT_NetHeader header;
   union
   {
      ECAT_NetHello hello;
      ECAT_NetCommand command;
      ECAT_NetSdoRequest sdo;
      uint8_t raw[1024];
   } payload;

   memset(&payload, 0, sizeof(payload));
   if (recv_message(client, &header, &payload, sizeof(payload)) != 0 ||
       header.type != ECAT_NET_MSG_HELLO)
   {
      return -1;
   }

   if (payload.hello.period_us > 0)
   {
      options.period_us = payload.hello.period_us;
   }

   runtime_init(&rt, &options);
   if (runtime_start(&rt) != 0)
   {
      runtime_destroy(&rt);
      return -1;
   }
   runtime_wait_first_status(&rt);

   if (send_runtime_status(client, &rt, ++rt.response_sequence) != 0)
   {
      runtime_stop_and_join(&rt);
      runtime_destroy(&rt);
      return -1;
   }

   while (!G_stop)
   {
      memset(&payload, 0, sizeof(payload));
      if (recv_message(client, &header, &payload, sizeof(payload)) != 0)
      {
         break;
      }
      runtime_mark_client_message(&rt);

      if (header.type == ECAT_NET_MSG_COMMAND)
      {
         runtime_apply_command(&rt, &payload.command);
         if (send_runtime_status(client, &rt, ++rt.response_sequence) != 0)
         {
            break;
         }
      }
      else if (header.type == ECAT_NET_MSG_SDO_READ)
      {
         ECAT_NetSdoReply reply;
         (void)runtime_read_cached_sdo(&rt, &payload.sdo, &reply);
         if (send_message(client, ECAT_NET_MSG_SDO_READ_REPLY,
                          header.sequence, &reply, sizeof(reply)) != 0)
         {
            break;
         }
      }
      else if (header.type == ECAT_NET_MSG_SDO_WRITE)
      {
         ECAT_NetSdoReply reply;
         (void)runtime_write_cached_sdo(&rt, &payload.sdo, &reply);
         if (send_message(client, ECAT_NET_MSG_SDO_WRITE_REPLY,
                          header.sequence, &reply, sizeof(reply)) != 0)
         {
            break;
         }
      }
      else
      {
         ECAT_NetError error;
         memset(&error, 0, sizeof(error));
         error.code = -1;
         safe_copy(error.message, sizeof(error.message),
                   "Unsupported message type.");
         if (send_message(client, ECAT_NET_MSG_ERROR, header.sequence,
                          &error, sizeof(error)) != 0)
         {
            break;
         }
      }
   }

   runtime_stop_and_join(&rt);
   runtime_destroy(&rt);
   return 0;
}

int main(int argc, char **argv)
{
   BackendOptions options;
   socket_t server;
   struct sockaddr_in addr;
   int opt = 1;
   int parse_result;

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
   signal(SIGPIPE, SIG_IGN);

   server = socket(AF_INET, SOCK_STREAM, 0);
   if (server == INVALID_SOCKET)
   {
      perror("socket");
      return 1;
   }

   (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   addr.sin_port = htons((uint16_t)options.port);

   if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
       listen(server, 1) == SOCKET_ERROR)
   {
      perror("bind/listen");
      close_socket(server);
      return 1;
   }

   printf("IgH LTS backend server listening on TCP port %d\n", options.port);
   printf("Build target: ethercat_igh_backend_server\n");
   printf("Press Ctrl+C to stop.\n");

   while (!G_stop)
   {
      socket_t client = accept(server, NULL, NULL);
      if (client == INVALID_SOCKET)
      {
         if (errno == EINTR)
         {
            continue;
         }
         break;
      }
      printf("client connected\n");
      (void)handle_client(client, &options);
      close_socket(client);
      printf("client disconnected\n");
   }

   close_socket(server);
   return 0;
}
