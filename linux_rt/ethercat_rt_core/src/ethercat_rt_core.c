#include "ecat_protocol.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RtMockCore
{
   int running;
   int period_us;
   uint32_t sequence;
   int32_t position;
   int32_t velocity;
   uint16_t controlword;
   uint16_t statusword;
   int8_t mode_display;
} RtMockCore;

static RtMockCore G_core;

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

static int socket_startup(void)
{
#ifdef _WIN32
   WSADATA data;
   return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
   signal(SIGPIPE, SIG_IGN);
   return 0;
#endif
}

static void socket_cleanup(void)
{
#ifdef _WIN32
   WSACleanup();
#endif
}

static int sleep_ms(int ms)
{
#ifdef _WIN32
   Sleep((DWORD)ms);
   return 0;
#else
   struct timespec ts;
   ts.tv_sec = ms / 1000;
   ts.tv_nsec = (long)(ms % 1000) * 1000000L;
   return nanosleep(&ts, NULL);
#endif
}

static int send_all(socket_t sock, const void *data, size_t size)
{
   const char *p = (const char *)data;
   size_t sent = 0;
   while (sent < size)
   {
      int n = send(sock, p + sent, (int)(size - sent), 0);
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
      int n = recv(sock, p + received, (int)(size - received), 0);
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

static void fill_status(ECAT_NetStatus *status)
{
   ECAT_NetSlaveStatus *slave;

   memset(status, 0, sizeof(*status));
   status->runtime.connected = G_core.running ? 1 : 0;
   status->runtime.operational = G_core.running ? 1 : 0;
   status->runtime.slave_count = 1;
   status->runtime.expected_wkc = 3;
   status->runtime.last_wkc = G_core.running ? 3 : 0;
   status->runtime.total_cycles = (int32_t)G_core.sequence;
   status->runtime.cycle_us = G_core.period_us;
   status->runtime.min_cycle_us = G_core.period_us;
   status->runtime.max_cycle_us = G_core.period_us + 5;
   status->runtime.avg_cycle_us = (double)G_core.period_us;
   status->runtime.period_us = G_core.period_us;
   safe_copy(status->runtime.state_text, sizeof(status->runtime.state_text),
             G_core.running ? "Linux RT Mock Operational"
                            : "Linux RT Mock Connected");
   safe_copy(status->runtime.crc_status, sizeof(status->runtime.crc_status),
             "RT core mock: Ethernet FCS/CRC not sampled.");

   slave = &status->slaves[0];
   slave->index = 1;
   safe_copy(slave->name, sizeof(slave->name), "LinuxRT Mock CiA402 Drive");
   slave->state = 0x08;
   slave->vendor_id = 0x00000001u;
   slave->product_code = 0x00001234u;
   slave->revision = 0x00000000u;
   slave->output_bytes = 8;
   slave->input_bytes = 8;
   slave->has_dc = 1;
   slave->controlword = G_core.controlword;
   slave->statusword = G_core.statusword;
   slave->mode_display = G_core.mode_display;
   slave->actual_position = G_core.position;
   slave->actual_velocity = G_core.velocity;
   slave->output_size = 8;
   slave->input_size = 8;
   slave->outputs[0] = (uint8_t)(G_core.controlword & 0xffu);
   slave->outputs[1] = (uint8_t)((G_core.controlword >> 8) & 0xffu);
   slave->inputs[0] = (uint8_t)(G_core.statusword & 0xffu);
   slave->inputs[1] = (uint8_t)((G_core.statusword >> 8) & 0xffu);
}

static void process_command(const ECAT_NetCommand *command)
{
   if (command == NULL)
   {
      return;
   }
   switch ((ECAT_NetCommandType)command->command)
   {
   case ECAT_NET_CMD_START:
      G_core.running = 1;
      G_core.statusword = 0x1237;
      break;
   case ECAT_NET_CMD_STOP:
      G_core.running = 0;
      G_core.velocity = 0;
      G_core.statusword = 0x0040;
      break;
   case ECAT_NET_CMD_RESET_STATISTICS:
      G_core.sequence = 0;
      break;
   case ECAT_NET_CMD_SERVO_ENABLE:
      G_core.running = 1;
      G_core.controlword = 0x000F;
      G_core.statusword = 0x1237;
      break;
   case ECAT_NET_CMD_SERVO_DISABLE:
      G_core.controlword = 0x0006;
      G_core.statusword = 0x0021;
      break;
   case ECAT_NET_CMD_SERVO_FAULT_RESET:
      G_core.controlword = 0x0080;
      G_core.statusword = 0x0040;
      break;
   case ECAT_NET_CMD_SERVO_SET_MODE:
      G_core.mode_display = (int8_t)command->mode;
      break;
   case ECAT_NET_CMD_SERVO_MOVE_ABS:
   case ECAT_NET_CMD_LMS_MOVE_ABS:
      G_core.running = 1;
      G_core.mode_display = 1;
      G_core.position = command->target_position;
      G_core.velocity = (int32_t)command->velocity;
      G_core.controlword = 0x003F;
      G_core.statusword = 0x1637;
      break;
   case ECAT_NET_CMD_SERVO_JOG:
   case ECAT_NET_CMD_LMS_MOVE_VEL:
      G_core.running = 1;
      G_core.mode_display = 3;
      G_core.velocity = command->target_velocity;
      G_core.position += command->target_velocity / 100;
      G_core.controlword = 0x000F;
      G_core.statusword = 0x1237;
      break;
   case ECAT_NET_CMD_SERVO_STOP:
   case ECAT_NET_CMD_LMS_STOP:
      G_core.velocity = 0;
      G_core.controlword = 0x010F;
      G_core.statusword = 0x1437;
      break;
   case ECAT_NET_CMD_SERVO_HOME:
      G_core.running = 1;
      G_core.mode_display = 6;
      G_core.position = 0;
      G_core.controlword = 0x001F;
      G_core.statusword = 0x1637;
      break;
   default:
      break;
   }
}

static int handle_client(socket_t client)
{
   ECAT_NetHeader header;
   union
   {
      ECAT_NetHello hello;
      ECAT_NetCommand command;
      ECAT_NetSdoRequest sdo;
      uint8_t raw[1024];
   } payload;
   ECAT_NetStatus status;

   memset(&payload, 0, sizeof(payload));
   if (recv_message(client, &header, &payload, sizeof(payload)) != 0 ||
       header.type != ECAT_NET_MSG_HELLO)
   {
      return -1;
   }

   G_core.period_us = payload.hello.period_us > 0
                         ? payload.hello.period_us
                         : 1000;
   G_core.running = payload.hello.request_operational ? 1 : 0;
   G_core.statusword = G_core.running ? 0x1237 : 0x0040;
   G_core.controlword = G_core.running ? 0x000F : 0x0000;

   fill_status(&status);
   if (send_message(client, ECAT_NET_MSG_STATUS, ++G_core.sequence,
                    &status, sizeof(status)) != 0)
   {
      return -1;
   }

   while (1)
   {
      memset(&payload, 0, sizeof(payload));
      if (recv_message(client, &header, &payload, sizeof(payload)) != 0)
      {
         return 0;
      }

      if (header.type == ECAT_NET_MSG_COMMAND)
      {
         process_command(&payload.command);
      }
      else if (header.type == ECAT_NET_MSG_SDO_READ)
      {
         ECAT_NetSdoReply reply;
         memset(&reply, 0, sizeof(reply));
         reply.result = 0;
         if (payload.sdo.index == 0x6041)
         {
            reply.size = 2;
            reply.data[0] = (uint8_t)(G_core.statusword & 0xffu);
            reply.data[1] = (uint8_t)((G_core.statusword >> 8) & 0xffu);
         }
         else if (payload.sdo.index == 0x6064)
         {
            reply.size = 4;
            memcpy(reply.data, &G_core.position, 4);
         }
         else
         {
            reply.result = -1;
         }
         (void)send_message(client, ECAT_NET_MSG_SDO_READ_REPLY,
                            header.sequence, &reply, sizeof(reply));
         continue;
      }
      else if (header.type == ECAT_NET_MSG_SDO_WRITE)
      {
         ECAT_NetSdoReply reply;
         memset(&reply, 0, sizeof(reply));
         reply.result = 0;
         if (payload.sdo.index == 0x6040 && payload.sdo.size >= 2)
         {
            G_core.controlword =
               (uint16_t)(payload.sdo.data[0] |
                          ((uint16_t)payload.sdo.data[1] << 8));
         }
         else if (payload.sdo.index == 0x6060 && payload.sdo.size >= 1)
         {
            G_core.mode_display = (int8_t)payload.sdo.data[0];
         }
         else
         {
            reply.result = -1;
         }
         (void)send_message(client, ECAT_NET_MSG_SDO_WRITE_REPLY,
                            header.sequence, &reply, sizeof(reply));
         continue;
      }

      if (G_core.running && G_core.velocity != 0)
      {
         G_core.position += G_core.velocity / 1000;
      }
      sleep_ms(1);
      fill_status(&status);
      if (send_message(client, ECAT_NET_MSG_STATUS, ++G_core.sequence,
                       &status, sizeof(status)) != 0)
      {
         return -1;
      }
   }
}

int main(int argc, char **argv)
{
   int port = ECAT_NET_DEFAULT_PORT;
   socket_t server;
   struct sockaddr_in addr;
   int opt = 1;

   if (argc > 1)
   {
      port = atoi(argv[1]);
      if (port <= 0)
      {
         port = ECAT_NET_DEFAULT_PORT;
      }
   }

   memset(&G_core, 0, sizeof(G_core));
   G_core.period_us = 1000;
   G_core.statusword = 0x0040;

   if (socket_startup() != 0)
   {
      fprintf(stderr, "socket startup failed\n");
      return 1;
   }

   server = socket(AF_INET, SOCK_STREAM, 0);
   if (server == INVALID_SOCKET)
   {
      fprintf(stderr, "socket failed\n");
      socket_cleanup();
      return 1;
   }

   (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
                    sizeof(opt));

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   addr.sin_port = htons((uint16_t)port);

   if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
       listen(server, 1) == SOCKET_ERROR)
   {
      fprintf(stderr, "bind/listen failed on port %d\n", port);
      close_socket(server);
      socket_cleanup();
      return 1;
   }

   printf("Linux RT mock core listening on TCP port %d\n", port);
   printf("Press Ctrl+C to stop.\n");

   while (1)
   {
      socket_t client = accept(server, NULL, NULL);
      if (client == INVALID_SOCKET)
      {
         break;
      }
      printf("client connected\n");
      (void)handle_client(client);
      close_socket(client);
      printf("client disconnected\n");
   }

   close_socket(server);
   socket_cleanup();
   return 0;
}
