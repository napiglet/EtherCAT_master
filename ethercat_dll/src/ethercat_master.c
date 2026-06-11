#include "ethercat_master.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ecat_protocol.h"
#include "soem/soem.h"

#define ECAT_DLL_VERSION "0.1.0"
#define ECAT_IOMAP_SIZE 4096
#define ECAT_DEFAULT_PERIOD_US 1000
#define ECAT_DB_INDEX_FILE "slaves.csv"
#define ECAT_DB_XML_DIR "xml"
#define ECAT_DEFAULT_LINUX_RT_HOST "127.0.0.1"
#define ECAT_DEFAULT_LINUX_RT_PORT ECAT_NET_DEFAULT_PORT
#define ECAT_INVALID_SOCKET_HANDLE ((UINT_PTR)(~(UINT_PTR)0))

typedef struct InternalSlaveSnapshot
{
   ECAT_SlaveInfo info;
   unsigned char outputs[ECAT_MAX_PDO_COPY];
   int output_size;
   unsigned char inputs[ECAT_MAX_PDO_COPY];
   int input_size;
} InternalSlaveSnapshot;

typedef struct EthercatCore
{
   ecx_contextt context;
   unsigned char iomap[ECAT_IOMAP_SIZE];
   CRITICAL_SECTION state_lock;
   CRITICAL_SECTION bus_lock;
   CRITICAL_SECTION db_lock;
   CRITICAL_SECTION rt_lock;
   HANDLE worker;
   volatile LONG stop_flag;
   volatile LONG worker_running;
   volatile LONG opened;
   char adapter_name[ECAT_MAX_ADAPTER_NAME];
   int request_operational;
   int period_us;
   int backend_type;
   char linux_rt_host[64];
   int linux_rt_port;
   UINT_PTR rt_socket;
   unsigned int rt_sequence;
   ECAT_RuntimeStatus status;
   InternalSlaveSnapshot slaves[EC_MAXSLAVE];
   ECAT_DbEntry db_entries[ECAT_MAX_DB_ENTRIES];
   int db_count;
   char db_root[ECAT_MAX_PATH_TEXT];
   ECAT_LogCallback log_callback;
} EthercatCore;

static EthercatCore G_core;
static INIT_ONCE G_init_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER G_qpc_freq;

static void set_default_db_root(void);
static void safe_copy(char *dst, size_t dst_size, const char *src);
static void set_state_text(const char *state_text, const char *last_error);
static int db_reload_locked(void);
static int db_find_match_locked(unsigned int vendor_id,
                                unsigned int product_code,
                                unsigned int revision,
                                ECAT_DbEntry *entry);
static int db_find_match_threadsafe(unsigned int vendor_id,
                                    unsigned int product_code,
                                    unsigned int revision,
                                    ECAT_DbEntry *entry);

static BOOL CALLBACK init_once_callback(PINIT_ONCE init_once, PVOID parameter,
                                        PVOID *context)
{
   (void)init_once;
   (void)parameter;
   (void)context;
   InitializeCriticalSection(&G_core.state_lock);
   InitializeCriticalSection(&G_core.bus_lock);
   InitializeCriticalSection(&G_core.db_lock);
   InitializeCriticalSection(&G_core.rt_lock);
   QueryPerformanceFrequency(&G_qpc_freq);
   G_core.period_us = ECAT_DEFAULT_PERIOD_US;
   G_core.backend_type = ECAT_BACKEND_WINDOWS_DEBUG;
   G_core.rt_socket = ECAT_INVALID_SOCKET_HANDLE;
   G_core.linux_rt_port = ECAT_DEFAULT_LINUX_RT_PORT;
   safe_copy(G_core.linux_rt_host, sizeof(G_core.linux_rt_host),
             ECAT_DEFAULT_LINUX_RT_HOST);
   {
      WSADATA data;
      (void)WSAStartup(MAKEWORD(2, 2), &data);
   }
   set_default_db_root();
   (void)db_reload_locked();
   strcpy_s(G_core.status.state_text, sizeof(G_core.status.state_text),
            "Disconnected");
   strcpy_s(G_core.status.crc_status, sizeof(G_core.status.crc_status),
            "Ethernet FCS/CRC is not exposed by Npcap/WinPcap.");
   return TRUE;
}

static void ensure_initialized(void)
{
   InitOnceExecuteOnce(&G_init_once, init_once_callback, NULL, NULL);
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

static int is_linux_rt_backend(void)
{
   return G_core.backend_type == ECAT_BACKEND_LINUX_RT;
}

static void rt_close_socket_locked(void)
{
   if (G_core.rt_socket != ECAT_INVALID_SOCKET_HANDLE)
   {
      closesocket(G_core.rt_socket);
      G_core.rt_socket = ECAT_INVALID_SOCKET_HANDLE;
   }
}

static int rt_send_all(UINT_PTR sock, const void *data, size_t size)
{
   const char *p = (const char *)data;
   size_t sent = 0;
   while (sent < size)
   {
      int n = send(sock, p + sent, (int)(size - sent), 0);
      if (n <= 0)
      {
         return ECAT_ERROR;
      }
      sent += (size_t)n;
   }
   return ECAT_OK;
}

static int rt_recv_all(UINT_PTR sock, void *data, size_t size)
{
   char *p = (char *)data;
   size_t received = 0;
   while (received < size)
   {
      int n = recv(sock, p + received, (int)(size - received), 0);
      if (n <= 0)
      {
         return ECAT_ERROR;
      }
      received += (size_t)n;
   }
   return ECAT_OK;
}

static int rt_send_message_locked(unsigned short type, const void *payload,
                                  unsigned int payload_size)
{
   ECAT_NetHeader header;

   if (G_core.rt_socket == ECAT_INVALID_SOCKET_HANDLE)
   {
      return ECAT_NOT_OPEN;
   }

   memset(&header, 0, sizeof(header));
   header.magic = ECAT_NET_MAGIC;
   header.version = ECAT_NET_VERSION;
   header.type = type;
   header.size = payload_size;
   header.sequence = ++G_core.rt_sequence;

   if (rt_send_all(G_core.rt_socket, &header, sizeof(header)) != ECAT_OK)
   {
      return ECAT_ERROR;
   }
   if (payload_size > 0 && payload != NULL)
   {
      return rt_send_all(G_core.rt_socket, payload, payload_size);
   }
   return ECAT_OK;
}

static int rt_recv_message_locked(ECAT_NetHeader *header, void *payload,
                                  unsigned int payload_capacity)
{
   if (G_core.rt_socket == ECAT_INVALID_SOCKET_HANDLE || header == NULL)
   {
      return ECAT_NOT_OPEN;
   }
   if (rt_recv_all(G_core.rt_socket, header, sizeof(*header)) != ECAT_OK)
   {
      return ECAT_ERROR;
   }
   if (header->magic != ECAT_NET_MAGIC ||
       header->version != ECAT_NET_VERSION ||
       header->size > payload_capacity)
   {
      return ECAT_ERROR;
   }
   if (header->size > 0 && payload != NULL)
   {
      return rt_recv_all(G_core.rt_socket, payload, header->size);
   }
   return ECAT_OK;
}

static void rt_apply_status_locked(const ECAT_NetStatus *net_status)
{
   int i;
   int slave_count;

   if (net_status == NULL)
   {
      return;
   }

   EnterCriticalSection(&G_core.state_lock);
   memset(&G_core.status, 0, sizeof(G_core.status));
   memset(G_core.slaves, 0, sizeof(G_core.slaves));

   G_core.status.connected = net_status->runtime.connected;
   G_core.status.operational = net_status->runtime.operational;
   G_core.status.slave_count = net_status->runtime.slave_count;
   G_core.status.expected_wkc = net_status->runtime.expected_wkc;
   G_core.status.last_wkc = net_status->runtime.last_wkc;
   G_core.status.total_cycles = net_status->runtime.total_cycles;
   G_core.status.wkc_errors = net_status->runtime.wkc_errors;
   G_core.status.state_errors = net_status->runtime.state_errors;
   G_core.status.cycle_us = net_status->runtime.cycle_us;
   G_core.status.min_cycle_us = net_status->runtime.min_cycle_us;
   G_core.status.max_cycle_us = net_status->runtime.max_cycle_us;
   G_core.status.avg_cycle_us = net_status->runtime.avg_cycle_us;
   G_core.status.period_us = net_status->runtime.period_us;
   G_core.status.dc_time_ns = net_status->runtime.dc_time_ns;
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             net_status->runtime.state_text);
   safe_copy(G_core.status.last_error, sizeof(G_core.status.last_error),
             net_status->runtime.last_error);
   safe_copy(G_core.status.crc_status, sizeof(G_core.status.crc_status),
             net_status->runtime.crc_status);

   slave_count = net_status->runtime.slave_count;
   if (slave_count > ECAT_NET_MAX_SLAVES)
   {
      slave_count = ECAT_NET_MAX_SLAVES;
   }
   if (slave_count > EC_MAXSLAVE - 1)
   {
      slave_count = EC_MAXSLAVE - 1;
   }

   for (i = 0; i < slave_count; ++i)
   {
      const ECAT_NetSlaveStatus *src = &net_status->slaves[i];
      InternalSlaveSnapshot *dst = &G_core.slaves[i];
      int out_len = src->output_size;
      int in_len = src->input_size;

      if (out_len > ECAT_MAX_PDO_COPY)
      {
         out_len = ECAT_MAX_PDO_COPY;
      }
      if (out_len > ECAT_NET_MAX_PDO_COPY)
      {
         out_len = ECAT_NET_MAX_PDO_COPY;
      }
      if (in_len > ECAT_MAX_PDO_COPY)
      {
         in_len = ECAT_MAX_PDO_COPY;
      }
      if (in_len > ECAT_NET_MAX_PDO_COPY)
      {
         in_len = ECAT_NET_MAX_PDO_COPY;
      }

      dst->info.index = src->index;
      safe_copy(dst->info.name, sizeof(dst->info.name), src->name);
      dst->info.state = src->state;
      dst->info.al_status = src->al_status;
      dst->info.vendor_id = src->vendor_id;
      dst->info.product_code = src->product_code;
      dst->info.revision = src->revision;
      dst->info.serial = src->serial;
      dst->info.output_bytes = src->output_bytes;
      dst->info.input_bytes = src->input_bytes;
      dst->info.has_dc = src->has_dc;
      dst->info.is_lost = src->is_lost;
      dst->output_size = out_len;
      dst->input_size = in_len;
      if (out_len > 0)
      {
         memcpy(dst->outputs, src->outputs, (size_t)out_len);
      }
      if (in_len > 0)
      {
         memcpy(dst->inputs, src->inputs, (size_t)in_len);
      }
      {
         ECAT_DbEntry db_entry;
         if (db_find_match_threadsafe(dst->info.vendor_id,
                                      dst->info.product_code,
                                      dst->info.revision,
                                      &db_entry) == ECAT_OK)
         {
            dst->info.database_matched = 1;
            safe_copy(dst->info.database_name,
                      sizeof(dst->info.database_name), db_entry.name);
            safe_copy(dst->info.database_xml,
                      sizeof(dst->info.database_xml), db_entry.xml_path);
         }
      }
   }

   LeaveCriticalSection(&G_core.state_lock);
}

static int rt_recv_status_locked(void)
{
   ECAT_NetHeader header;
   ECAT_NetStatus status;
   int result;

   memset(&status, 0, sizeof(status));
   result = rt_recv_message_locked(&header, &status, sizeof(status));
   if (result != ECAT_OK)
   {
      return result;
   }
   if (header.type != ECAT_NET_MSG_STATUS)
   {
      return ECAT_ERROR;
   }
   rt_apply_status_locked(&status);
   return ECAT_OK;
}

static int rt_connect_locked(void)
{
   struct addrinfo hints;
   struct addrinfo *results = NULL;
   struct addrinfo *it;
   char port_text[16];
   int result = ECAT_ERROR;
   DWORD timeout_ms = 500;

   rt_close_socket_locked();

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   (void)snprintf(port_text, sizeof(port_text), "%d", G_core.linux_rt_port);

   if (getaddrinfo(G_core.linux_rt_host, port_text, &hints, &results) != 0)
   {
      return ECAT_ERROR;
   }

   for (it = results; it != NULL; it = it->ai_next)
   {
      UINT_PTR sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
      if (sock == ECAT_INVALID_SOCKET_HANDLE)
      {
         continue;
      }
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                 sizeof(timeout_ms));
      setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms,
                 sizeof(timeout_ms));
      if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0)
      {
         G_core.rt_socket = sock;
         result = ECAT_OK;
         break;
      }
      closesocket(sock);
   }

   freeaddrinfo(results);
   return result;
}

static int rt_send_command_locked(const ECAT_NetCommand *command)
{
   int result;

   result = rt_send_message_locked(ECAT_NET_MSG_COMMAND, command,
                                   sizeof(*command));
   if (result != ECAT_OK)
   {
      return result;
   }
   return rt_recv_status_locked();
}

static int rt_poll_status(void)
{
   ECAT_NetCommand command;
   int result;

   memset(&command, 0, sizeof(command));
   EnterCriticalSection(&G_core.rt_lock);
   result = rt_send_command_locked(&command);
   if (result != ECAT_OK)
   {
      rt_close_socket_locked();
      InterlockedExchange(&G_core.opened, 0);
      InterlockedExchange(&G_core.worker_running, 0);
   }
   LeaveCriticalSection(&G_core.rt_lock);
   return result;
}

static int rt_open_backend(const ECAT_OpenOptions *options)
{
   ECAT_NetHello hello;
   int result;

   memset(&hello, 0, sizeof(hello));
   safe_copy(hello.client_name, sizeof(hello.client_name),
             "ethercat_dll_windows_client");
   hello.period_us =
      (options != NULL && options->period_us > 0)
         ? options->period_us
         : ECAT_DEFAULT_PERIOD_US;
   hello.request_operational =
      (options != NULL) ? options->request_operational : 0;

   EnterCriticalSection(&G_core.rt_lock);
   result = rt_connect_locked();
   if (result == ECAT_OK)
   {
      result = rt_send_message_locked(ECAT_NET_MSG_HELLO, &hello,
                                      sizeof(hello));
   }
   if (result == ECAT_OK)
   {
      result = rt_recv_status_locked();
   }
   if (result == ECAT_OK)
   {
      InterlockedExchange(&G_core.opened, 1);
      InterlockedExchange(&G_core.worker_running, 1);
      safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
                "Linux RT backend connected");
   }
   else
   {
      rt_close_socket_locked();
      InterlockedExchange(&G_core.opened, 0);
      InterlockedExchange(&G_core.worker_running, 0);
      set_state_text("Disconnected", "Linux RT backend connection failed.");
   }
   LeaveCriticalSection(&G_core.rt_lock);
   return result;
}

static int rt_close_backend(void)
{
   ECAT_NetCommand command;

   memset(&command, 0, sizeof(command));
   command.command = ECAT_NET_CMD_STOP;

   EnterCriticalSection(&G_core.rt_lock);
   if (G_core.rt_socket != ECAT_INVALID_SOCKET_HANDLE)
   {
      (void)rt_send_command_locked(&command);
   }
   rt_close_socket_locked();
   InterlockedExchange(&G_core.opened, 0);
   InterlockedExchange(&G_core.worker_running, 0);
   LeaveCriticalSection(&G_core.rt_lock);

   EnterCriticalSection(&G_core.state_lock);
   G_core.status.connected = 0;
   G_core.status.operational = 0;
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             "Disconnected");
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

static int rt_send_motion_command(ECAT_NetCommandType type, int slave_index,
                                  int target_position, int target_velocity,
                                  unsigned int velocity,
                                  unsigned int acceleration,
                                  unsigned int deceleration,
                                  int mode, int homing_method,
                                  unsigned int search_speed,
                                  unsigned int latch_speed)
{
   ECAT_NetCommand command;
   int result;

   memset(&command, 0, sizeof(command));
   command.command = (int)type;
   command.slave_index = slave_index;
   command.target_position = target_position;
   command.target_velocity = target_velocity;
   command.velocity = velocity;
   command.acceleration = acceleration;
   command.deceleration = deceleration;
   command.mode = mode;
   command.homing_method = homing_method;
   command.search_speed = search_speed;
   command.latch_speed = latch_speed;

   EnterCriticalSection(&G_core.rt_lock);
   result = rt_send_command_locked(&command);
   LeaveCriticalSection(&G_core.rt_lock);
   return result;
}

static int rt_read_sdo(int slave_index, unsigned short index,
                       unsigned char subindex, unsigned char *data,
                       int data_capacity, int *data_size)
{
   ECAT_NetSdoRequest request;
   ECAT_NetSdoReply reply;
   ECAT_NetHeader header;
   int result;
   int copy;

   if (data == NULL || data_capacity <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   memset(&request, 0, sizeof(request));
   request.slave_index = slave_index;
   request.index = index;
   request.subindex = subindex;
   request.size = data_capacity;

   EnterCriticalSection(&G_core.rt_lock);
   result = rt_send_message_locked(ECAT_NET_MSG_SDO_READ, &request,
                                   sizeof(request));
   if (result == ECAT_OK)
   {
      result = rt_recv_message_locked(&header, &reply, sizeof(reply));
   }
   LeaveCriticalSection(&G_core.rt_lock);

   if (result != ECAT_OK)
   {
      return result;
   }
   if (header.type != ECAT_NET_MSG_SDO_READ_REPLY || reply.result != ECAT_OK)
   {
      return ECAT_ERROR;
   }

   copy = reply.size < data_capacity ? reply.size : data_capacity;
   if (copy > 0)
   {
      memcpy(data, reply.data, (size_t)copy);
   }
   if (data_size != NULL)
   {
      *data_size = copy;
   }
   return ECAT_OK;
}

static int rt_write_sdo(int slave_index, unsigned short index,
                        unsigned char subindex, const unsigned char *data,
                        int data_size)
{
   ECAT_NetSdoRequest request;
   ECAT_NetSdoReply reply;
   ECAT_NetHeader header;
   int result;

   if (data == NULL || data_size <= 0 || data_size > (int)sizeof(request.data))
   {
      return ECAT_INVALID_ARGUMENT;
   }

   memset(&request, 0, sizeof(request));
   request.slave_index = slave_index;
   request.index = index;
   request.subindex = subindex;
   request.size = data_size;
   memcpy(request.data, data, (size_t)data_size);

   EnterCriticalSection(&G_core.rt_lock);
   result = rt_send_message_locked(ECAT_NET_MSG_SDO_WRITE, &request,
                                   sizeof(request));
   if (result == ECAT_OK)
   {
      result = rt_recv_message_locked(&header, &reply, sizeof(reply));
   }
   LeaveCriticalSection(&G_core.rt_lock);

   if (result != ECAT_OK)
   {
      return result;
   }
   if (header.type != ECAT_NET_MSG_SDO_WRITE_REPLY || reply.result != ECAT_OK)
   {
      return ECAT_ERROR;
   }
   return ECAT_OK;
}

static const char *find_text_i(const char *haystack, const char *needle)
{
   size_t needle_len;

   if (haystack == NULL || needle == NULL)
   {
      return NULL;
   }
   needle_len = strlen(needle);
   if (needle_len == 0)
   {
      return haystack;
   }

   while (*haystack != '\0')
   {
      size_t i;
      for (i = 0; i < needle_len; ++i)
      {
         unsigned char a = (unsigned char)haystack[i];
         unsigned char b = (unsigned char)needle[i];
         if (a == '\0' || tolower(a) != tolower(b))
         {
            break;
         }
      }
      if (i == needle_len)
      {
         return haystack;
      }
      ++haystack;
   }
   return NULL;
}

static void trim_in_place(char *text)
{
   char *start;
   char *end;

   if (text == NULL)
   {
      return;
   }

   start = text;
   while (*start != '\0' && isspace((unsigned char)*start))
   {
      ++start;
   }
   if (start != text)
   {
      memmove(text, start, strlen(start) + 1);
   }

   end = text + strlen(text);
   while (end > text && isspace((unsigned char)end[-1]))
   {
      --end;
   }
   *end = '\0';
}

static void sanitize_csv_text(char *text)
{
   char *p = text;
   while (p != NULL && *p != '\0')
   {
      if (*p == ',' || *p == '\r' || *p == '\n')
      {
         *p = ' ';
      }
      ++p;
   }
   trim_in_place(text);
}

static void sanitize_file_text(char *text)
{
   char *p = text;
   while (p != NULL && *p != '\0')
   {
      if (*p == '<' || *p == '>' || *p == ':' || *p == '"' || *p == '/' ||
          *p == '\\' || *p == '|' || *p == '?' || *p == '*' ||
          (unsigned char)*p < 32)
      {
         *p = '_';
      }
      ++p;
   }
}

static void path_join(char *dst, size_t dst_size, const char *left,
                      const char *right)
{
   size_t len;

   if (dst_size == 0)
   {
      return;
   }
   if (left == NULL || left[0] == '\0')
   {
      safe_copy(dst, dst_size, right);
      return;
   }
   if (right == NULL || right[0] == '\0')
   {
      safe_copy(dst, dst_size, left);
      return;
   }

   len = strlen(left);
   if (left[len - 1] == '\\' || left[len - 1] == '/')
   {
      (void)snprintf(dst, dst_size, "%s%s", left, right);
   }
   else
   {
      (void)snprintf(dst, dst_size, "%s\\%s", left, right);
   }
}

static const char *path_basename(const char *path)
{
   const char *slash;
   const char *backslash;

   if (path == NULL)
   {
      return "";
   }
   slash = strrchr(path, '/');
   backslash = strrchr(path, '\\');
   if (slash == NULL || backslash > slash)
   {
      slash = backslash;
   }
   return slash != NULL ? slash + 1 : path;
}

static int make_directory_if_needed(const char *path)
{
   if (path == NULL || path[0] == '\0')
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (CreateDirectoryA(path, NULL) ||
       GetLastError() == ERROR_ALREADY_EXISTS)
   {
      return ECAT_OK;
   }
   return ECAT_ERROR;
}

static void set_default_db_root(void)
{
   char exe_path[MAX_PATH];
   char exe_dir[MAX_PATH];
   char *last_slash;
   DWORD len;

   len = GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path));
   if (len == 0 || len >= sizeof(exe_path))
   {
      safe_copy(G_core.db_root, sizeof(G_core.db_root), ".\\ethercat_db");
      return;
   }

   safe_copy(exe_dir, sizeof(exe_dir), exe_path);
   last_slash = strrchr(exe_dir, '\\');
   if (last_slash == NULL)
   {
      last_slash = strrchr(exe_dir, '/');
   }
   if (last_slash != NULL)
   {
      *last_slash = '\0';
   }
   else
   {
      safe_copy(exe_dir, sizeof(exe_dir), ".");
   }
   path_join(G_core.db_root, sizeof(G_core.db_root), exe_dir, "ethercat_db");
}

static int ensure_db_dirs_locked(void)
{
   char xml_dir[ECAT_MAX_PATH_TEXT];
   int result;

   result = make_directory_if_needed(G_core.db_root);
   if (result != ECAT_OK)
   {
      return result;
   }
   path_join(xml_dir, sizeof(xml_dir), G_core.db_root, ECAT_DB_XML_DIR);
   return make_directory_if_needed(xml_dir);
}

static void db_index_path_locked(char *dst, size_t dst_size)
{
   path_join(dst, dst_size, G_core.db_root, ECAT_DB_INDEX_FILE);
}

static void db_xml_dir_locked(char *dst, size_t dst_size)
{
   path_join(dst, dst_size, G_core.db_root, ECAT_DB_XML_DIR);
}

static unsigned int parse_ecat_number(const char *text)
{
   const char *p = text;
   int base = 10;

   if (p == NULL)
   {
      return 0;
   }
   while (*p != '\0' && isspace((unsigned char)*p))
   {
      ++p;
   }
   if (p[0] == '#' && (p[1] == 'x' || p[1] == 'X'))
   {
      p += 2;
      base = 16;
   }
   else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
   {
      p += 2;
      base = 16;
   }
   return (unsigned int)strtoul(p, NULL, base);
}

static int copy_attr_value(const char *xml, const char *attr, char *dst,
                           size_t dst_size)
{
   const char *p = xml;
   size_t attr_len;

   if (dst_size == 0)
   {
      return 0;
   }
   dst[0] = '\0';
   if (xml == NULL || attr == NULL)
   {
      return 0;
   }

   attr_len = strlen(attr);
   while ((p = find_text_i(p, attr)) != NULL)
   {
      const char *q = p + attr_len;
      const char *end;
      char quote = '\0';
      size_t len;

      while (*q != '\0' && isspace((unsigned char)*q))
      {
         ++q;
      }
      if (*q != '=')
      {
         p += attr_len;
         continue;
      }
      ++q;
      while (*q != '\0' && isspace((unsigned char)*q))
      {
         ++q;
      }
      if (*q == '"' || *q == '\'')
      {
         quote = *q;
         ++q;
      }
      end = q;
      while (*end != '\0' &&
             ((quote != '\0' && *end != quote) ||
              (quote == '\0' && !isspace((unsigned char)*end) && *end != '>')))
      {
         ++end;
      }
      len = (size_t)(end - q);
      if (len >= dst_size)
      {
         len = dst_size - 1;
      }
      memcpy(dst, q, len);
      dst[len] = '\0';
      trim_in_place(dst);
      return dst[0] != '\0';
   }
   return 0;
}

static int copy_tag_text_from(const char *xml, const char *tag, char *dst,
                              size_t dst_size)
{
   char open_tag[64];
   char close_tag[64];
   const char *start;
   const char *end;
   size_t len;

   if (dst_size == 0)
   {
      return 0;
   }
   dst[0] = '\0';
   if (xml == NULL || tag == NULL)
   {
      return 0;
   }

   (void)snprintf(open_tag, sizeof(open_tag), "<%s", tag);
   (void)snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
   start = find_text_i(xml, open_tag);
   if (start == NULL)
   {
      return 0;
   }
   start = strchr(start, '>');
   if (start == NULL)
   {
      return 0;
   }
   ++start;
   end = find_text_i(start, close_tag);
   if (end == NULL)
   {
      return 0;
   }

   len = (size_t)(end - start);
   if (len >= dst_size)
   {
      len = dst_size - 1;
   }
   memcpy(dst, start, len);
   dst[len] = '\0';
   trim_in_place(dst);
   return dst[0] != '\0';
}

static const char *find_xml_element(const char *xml, const char *tag)
{
   char open_tag[64];
   const char *p;
   size_t len;

   if (xml == NULL || tag == NULL)
   {
      return NULL;
   }

   (void)snprintf(open_tag, sizeof(open_tag), "<%s", tag);
   len = strlen(open_tag);
   p = xml;
   while ((p = find_text_i(p, open_tag)) != NULL)
   {
      char next = p[len];
      if (next == '\0' || next == '>' || next == '/' ||
          isspace((unsigned char)next))
      {
         return p;
      }
      p += len;
   }
   return NULL;
}

static int copy_vendor_id(const char *xml, char *dst, size_t dst_size)
{
   const char *vendor;
   const char *vendor_end;
   const char *id_start;

   if (copy_attr_value(xml, "VendorId", dst, dst_size) ||
       copy_attr_value(xml, "VendorID", dst, dst_size))
   {
      return 1;
   }

   vendor = find_xml_element(xml, "Vendor");
   if (vendor == NULL)
   {
      return copy_tag_text_from(xml, "VendorId", dst, dst_size) ||
             copy_tag_text_from(xml, "VendorID", dst, dst_size) ||
             copy_tag_text_from(xml, "Id", dst, dst_size);
   }
   vendor_end = find_text_i(vendor, "</Vendor>");
   id_start = find_text_i(vendor, "<Id");
   if (id_start == NULL || (vendor_end != NULL && id_start > vendor_end))
   {
      return copy_tag_text_from(xml, "VendorId", dst, dst_size) ||
             copy_tag_text_from(xml, "VendorID", dst, dst_size);
   }
   return copy_tag_text_from(id_start, "Id", dst, dst_size);
}

static int read_file_text(const char *path, char **buffer)
{
   FILE *fp;
   long size;
   char *data;
   size_t read_count;

   if (buffer == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   *buffer = NULL;
   fp = fopen(path, "rb");
   if (fp == NULL)
   {
      return ECAT_ERROR;
   }
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return ECAT_ERROR;
   }
   size = ftell(fp);
   if (size <= 0 || size > 16L * 1024L * 1024L)
   {
      fclose(fp);
      return ECAT_ERROR;
   }
   rewind(fp);

   data = (char *)malloc((size_t)size + 1U);
   if (data == NULL)
   {
      fclose(fp);
      return ECAT_ERROR;
   }
   read_count = fread(data, 1, (size_t)size, fp);
   fclose(fp);
   if (read_count != (size_t)size)
   {
      free(data);
      return ECAT_ERROR;
   }
   data[size] = '\0';
   *buffer = data;
   return ECAT_OK;
}

static void current_time_text(char *dst, size_t dst_size)
{
   time_t now;
   struct tm local_tm;

   if (dst_size == 0)
   {
      return;
   }
   now = time(NULL);
   if (localtime_s(&local_tm, &now) != 0)
   {
      safe_copy(dst, dst_size, "");
      return;
   }
   (void)strftime(dst, dst_size, "%Y-%m-%d %H:%M:%S", &local_tm);
}

static void strip_cdata(char *text)
{
   const char *prefix = "<![CDATA[";
   char *end;
   size_t prefix_len = strlen(prefix);

   trim_in_place(text);
   if (strncmp(text, prefix, prefix_len) != 0)
   {
      return;
   }

   memmove(text, text + prefix_len, strlen(text + prefix_len) + 1);
   end = strstr(text, "]]>");
   if (end != NULL)
   {
      *end = '\0';
   }
   trim_in_place(text);
}

static int parse_xml_device_from(const char *xml, const char *section,
                                 ECAT_DbEntry *entry)
{
   char value[ECAT_MAX_MESSAGE];
   const char *scan;

   if (xml == NULL || entry == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   scan = section != NULL ? section : xml;
   memset(entry, 0, sizeof(*entry));
   if (copy_vendor_id(scan, value, sizeof(value)) ||
       copy_vendor_id(xml, value, sizeof(value)))
   {
      entry->vendor_id = parse_ecat_number(value);
   }
   if (copy_attr_value(scan, "ProductCode", value, sizeof(value)) ||
       copy_tag_text_from(scan, "ProductCode", value, sizeof(value)))
   {
      entry->product_code = parse_ecat_number(value);
   }
   if (copy_attr_value(scan, "RevisionNo", value, sizeof(value)) ||
       copy_attr_value(scan, "Revision", value, sizeof(value)) ||
       copy_tag_text_from(scan, "RevisionNo", value, sizeof(value)))
   {
      entry->revision = parse_ecat_number(value);
   }

   if (!copy_tag_text_from(scan, "Name", entry->name, sizeof(entry->name)) &&
       !copy_tag_text_from(scan, "Type", entry->name, sizeof(entry->name)))
   {
      (void)snprintf(entry->name, sizeof(entry->name),
                     "Vendor 0x%08X Product 0x%08X",
                     entry->vendor_id, entry->product_code);
   }
   strip_cdata(entry->name);

   if (find_text_i(xml, "<EtherCATInfo") != NULL)
   {
      safe_copy(entry->xml_type, sizeof(entry->xml_type), "ESI");
   }
   else if (find_text_i(xml, "<EtherCATConfig") != NULL ||
            find_text_i(xml, "<Config") != NULL)
   {
      safe_copy(entry->xml_type, sizeof(entry->xml_type), "ENI");
   }
   else
   {
      safe_copy(entry->xml_type, sizeof(entry->xml_type), "XML");
   }

   sanitize_csv_text(entry->name);
   if (entry->vendor_id == 0 || entry->product_code == 0)
   {
      return ECAT_ERROR;
   }
   return ECAT_OK;
}

static int parse_xml_device(const char *xml, ECAT_DbEntry *entry)
{
   const char *section;

   section = find_xml_element(xml, "Slave");
   if (section == NULL)
   {
      section = find_xml_element(xml, "Device");
   }
   return parse_xml_device_from(xml, section, entry);
}

static void build_xml_store_path_locked(const ECAT_DbEntry *entry,
                                        const char *source_path, char *dst,
                                        size_t dst_size)
{
   char xml_dir[ECAT_MAX_PATH_TEXT];
   char source_name[96];
   char store_name[160];

   db_xml_dir_locked(xml_dir, sizeof(xml_dir));
   safe_copy(source_name, sizeof(source_name), path_basename(source_path));
   sanitize_file_text(source_name);
   (void)snprintf(store_name, sizeof(store_name), "%08X_%08X_%08X_%s",
                  entry->vendor_id, entry->product_code, entry->revision,
                  source_name[0] != '\0' ? source_name : "device.xml");
   sanitize_file_text(store_name);
   path_join(dst, dst_size, xml_dir, store_name);
}

static int db_save_locked(void)
{
   char index_path[ECAT_MAX_PATH_TEXT];
   FILE *fp;
   int i;

   if (ensure_db_dirs_locked() != ECAT_OK)
   {
      return ECAT_ERROR;
   }

   db_index_path_locked(index_path, sizeof(index_path));
   fp = fopen(index_path, "wb");
   if (fp == NULL)
   {
      return ECAT_ERROR;
   }
   fprintf(fp,
           "vendor_id,product_code,revision,name,xml_type,xml_path,imported_at\n");
   for (i = 0; i < G_core.db_count; ++i)
   {
      ECAT_DbEntry entry = G_core.db_entries[i];
      sanitize_csv_text(entry.name);
      sanitize_csv_text(entry.xml_type);
      sanitize_csv_text(entry.xml_path);
      sanitize_csv_text(entry.imported_at);
      fprintf(fp, "0x%08X,0x%08X,0x%08X,%s,%s,%s,%s\n",
              entry.vendor_id, entry.product_code, entry.revision,
              entry.name, entry.xml_type, entry.xml_path, entry.imported_at);
   }
   fclose(fp);
   return ECAT_OK;
}

static int db_load_line_locked(char *line)
{
   char *token;
   ECAT_DbEntry entry;
   int field = 0;

   if (line == NULL || line[0] == '\0' || line[0] == '#')
   {
      return ECAT_OK;
   }
   if (find_text_i(line, "vendor_id") == line)
   {
      return ECAT_OK;
   }

   memset(&entry, 0, sizeof(entry));
   token = strtok(line, ",");
   while (token != NULL)
   {
      trim_in_place(token);
      switch (field)
      {
      case 0:
         entry.vendor_id = parse_ecat_number(token);
         break;
      case 1:
         entry.product_code = parse_ecat_number(token);
         break;
      case 2:
         entry.revision = parse_ecat_number(token);
         break;
      case 3:
         safe_copy(entry.name, sizeof(entry.name), token);
         break;
      case 4:
         safe_copy(entry.xml_type, sizeof(entry.xml_type), token);
         break;
      case 5:
         safe_copy(entry.xml_path, sizeof(entry.xml_path), token);
         break;
      case 6:
         safe_copy(entry.imported_at, sizeof(entry.imported_at), token);
         break;
      default:
         break;
      }
      ++field;
      token = strtok(NULL, ",");
   }

   if (entry.vendor_id == 0 || entry.product_code == 0 ||
       G_core.db_count >= ECAT_MAX_DB_ENTRIES)
   {
      return ECAT_OK;
   }
   G_core.db_entries[G_core.db_count++] = entry;
   return ECAT_OK;
}

static int db_reload_locked(void)
{
   char index_path[ECAT_MAX_PATH_TEXT];
   FILE *fp;
   char line[1024];

   if (G_core.db_root[0] == '\0')
   {
      set_default_db_root();
   }

   if (ensure_db_dirs_locked() != ECAT_OK)
   {
      return ECAT_ERROR;
   }

   G_core.db_count = 0;
   memset(G_core.db_entries, 0, sizeof(G_core.db_entries));
   db_index_path_locked(index_path, sizeof(index_path));
   fp = fopen(index_path, "rb");
   if (fp == NULL)
   {
      return db_save_locked();
   }

   while (fgets(line, sizeof(line), fp) != NULL)
   {
      trim_in_place(line);
      (void)db_load_line_locked(line);
   }
   fclose(fp);
   return ECAT_OK;
}

static int db_upsert_locked(const ECAT_DbEntry *entry)
{
   int i;

   if (entry == NULL || entry->vendor_id == 0 || entry->product_code == 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   for (i = 0; i < G_core.db_count; ++i)
   {
      ECAT_DbEntry *current = &G_core.db_entries[i];
      if (current->vendor_id == entry->vendor_id &&
          current->product_code == entry->product_code &&
          current->revision == entry->revision)
      {
         *current = *entry;
         return ECAT_OK;
      }
   }

   if (G_core.db_count >= ECAT_MAX_DB_ENTRIES)
   {
      return ECAT_ERROR;
   }
   G_core.db_entries[G_core.db_count++] = *entry;
   return ECAT_OK;
}

static int db_find_match_locked(unsigned int vendor_id,
                                unsigned int product_code,
                                unsigned int revision,
                                ECAT_DbEntry *entry)
{
   int i;
   int fallback = -1;

   for (i = 0; i < G_core.db_count; ++i)
   {
      ECAT_DbEntry *candidate = &G_core.db_entries[i];
      if (candidate->vendor_id != vendor_id ||
          candidate->product_code != product_code)
      {
         continue;
      }
      if (candidate->revision == revision)
      {
         if (entry != NULL)
         {
            *entry = *candidate;
         }
         return ECAT_OK;
      }
      if (fallback < 0 || candidate->revision == 0)
      {
         fallback = i;
      }
   }

   if (fallback >= 0)
   {
      if (entry != NULL)
      {
         *entry = G_core.db_entries[fallback];
      }
      return ECAT_OK;
   }
   return ECAT_ERROR;
}

static int db_find_match_threadsafe(unsigned int vendor_id,
                                    unsigned int product_code,
                                    unsigned int revision,
                                    ECAT_DbEntry *entry)
{
   int result;

   EnterCriticalSection(&G_core.db_lock);
   result = db_find_match_locked(vendor_id, product_code, revision, entry);
   LeaveCriticalSection(&G_core.db_lock);
   return result;
}

static int db_upsert_xml_entries_locked(const char *xml_text,
                                        const char *stored_path,
                                        ECAT_DbEntry *first_entry)
{
   const char *section;
   const char *tag = "Slave";
   int imported_count = 0;
   int result = ECAT_ERROR;

   section = find_xml_element(xml_text, "Slave");
   if (section == NULL)
   {
      section = find_xml_element(xml_text, "Device");
      tag = "Device";
   }

   if (section == NULL)
   {
      ECAT_DbEntry entry;
      result = parse_xml_device(xml_text, &entry);
      if (result == ECAT_OK)
      {
         safe_copy(entry.xml_path, sizeof(entry.xml_path), stored_path);
         current_time_text(entry.imported_at, sizeof(entry.imported_at));
         result = db_upsert_locked(&entry);
         if (result == ECAT_OK && first_entry != NULL)
         {
            *first_entry = entry;
         }
      }
      return result;
   }

   while (section != NULL && imported_count < ECAT_MAX_DB_ENTRIES)
   {
      ECAT_DbEntry entry;
      result = parse_xml_device_from(xml_text, section, &entry);
      if (result == ECAT_OK)
      {
         safe_copy(entry.xml_path, sizeof(entry.xml_path), stored_path);
         current_time_text(entry.imported_at, sizeof(entry.imported_at));
         result = db_upsert_locked(&entry);
         if (result != ECAT_OK)
         {
            return result;
         }
         if (imported_count == 0 && first_entry != NULL)
         {
            *first_entry = entry;
         }
         ++imported_count;
      }
      section = find_xml_element(section + 1, tag);
   }

   return imported_count > 0 ? ECAT_OK : ECAT_ERROR;
}

static void log_message(int level, const char *message)
{
   ECAT_LogCallback callback;
   ensure_initialized();
   EnterCriticalSection(&G_core.state_lock);
   callback = G_core.log_callback;
   LeaveCriticalSection(&G_core.state_lock);
   if (callback != NULL)
   {
      callback(level, message);
   }
}

static void set_state_text(const char *state_text, const char *last_error)
{
   EnterCriticalSection(&G_core.state_lock);
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             state_text);
   if (last_error != NULL)
   {
      safe_copy(G_core.status.last_error, sizeof(G_core.status.last_error),
                last_error);
   }
   LeaveCriticalSection(&G_core.state_lock);
}

static int elapsed_us(LARGE_INTEGER start, LARGE_INTEGER end)
{
   return (int)(((end.QuadPart - start.QuadPart) * 1000000LL) /
                G_qpc_freq.QuadPart);
}

const char *ECAT_StateName(unsigned short state)
{
   switch (state & 0x0f)
   {
   case EC_STATE_INIT:
      return (state & EC_STATE_ERROR) ? "INIT+ERROR" : "INIT";
   case EC_STATE_PRE_OP:
      return (state & EC_STATE_ERROR) ? "PRE-OP+ERROR" : "PRE-OP";
   case EC_STATE_BOOT:
      return (state & EC_STATE_ERROR) ? "BOOT+ERROR" : "BOOT";
   case EC_STATE_SAFE_OP:
      return (state & EC_STATE_ERROR) ? "SAFE-OP+ERROR" : "SAFE-OP";
   case EC_STATE_OPERATIONAL:
      return (state & EC_STATE_ERROR) ? "OP+ERROR" : "OP";
   default:
      return (state & EC_STATE_ERROR) ? "ERROR" : "NONE";
   }
}

const char *ECAT_Cia402StateName(unsigned short statusword)
{
   if ((statusword & 0x004f) == 0x0000)
      return "Not ready";
   if ((statusword & 0x004f) == 0x0040)
      return "Switch on disabled";
   if ((statusword & 0x006f) == 0x0021)
      return "Ready to switch on";
   if ((statusword & 0x006f) == 0x0023)
      return "Switched on";
   if ((statusword & 0x006f) == 0x0027)
      return "Operation enabled";
   if ((statusword & 0x006f) == 0x0007)
      return "Quick stop active";
   if ((statusword & 0x004f) == 0x000f)
      return "Fault reaction";
   if ((statusword & 0x004f) == 0x0008)
      return "Fault";
   return "Unknown";
}

static void update_slave_snapshot_locked(ecx_contextt *context)
{
   int i;

   EnterCriticalSection(&G_core.state_lock);
   memset(G_core.slaves, 0, sizeof(G_core.slaves));
   G_core.status.slave_count = context->slavecount;

   for (i = 1; i <= context->slavecount && i < EC_MAXSLAVE; ++i)
   {
      ec_slavet *src = &context->slavelist[i];
      InternalSlaveSnapshot *dst = &G_core.slaves[i - 1];
      int out_len = (src->Obytes > ECAT_MAX_PDO_COPY)
                       ? ECAT_MAX_PDO_COPY
                       : (int)src->Obytes;
      int in_len = (src->Ibytes > ECAT_MAX_PDO_COPY)
                      ? ECAT_MAX_PDO_COPY
                      : (int)src->Ibytes;

      dst->info.index = i;
      safe_copy(dst->info.name, sizeof(dst->info.name), src->name);
      dst->info.state = src->state;
      dst->info.al_status = src->ALstatuscode;
      dst->info.config_address = src->configadr;
      dst->info.alias_address = src->aliasadr;
      dst->info.vendor_id = src->eep_man;
      dst->info.product_code = src->eep_id;
      dst->info.revision = src->eep_rev;
      dst->info.serial = src->eep_ser;
      dst->info.output_bits = src->Obits;
      dst->info.output_bytes = src->Obytes;
      dst->info.input_bits = src->Ibits;
      dst->info.input_bytes = src->Ibytes;
      dst->info.mailbox_write_bytes = src->mbx_l;
      dst->info.mailbox_read_bytes = src->mbx_rl;
      dst->info.mailbox_protocols = src->mbx_proto;
      dst->info.has_dc = src->hasdc ? 1 : 0;
      dst->info.is_lost = src->islost ? 1 : 0;
      {
         ECAT_DbEntry db_entry;
         if (db_find_match_threadsafe(dst->info.vendor_id,
                                      dst->info.product_code,
                                      dst->info.revision,
                                      &db_entry) == ECAT_OK)
         {
            dst->info.database_matched = 1;
            safe_copy(dst->info.database_name,
                      sizeof(dst->info.database_name), db_entry.name);
            safe_copy(dst->info.database_xml,
                      sizeof(dst->info.database_xml), db_entry.xml_path);
         }
      }

      if (src->outputs != NULL && out_len > 0)
      {
         memcpy(dst->outputs, src->outputs, (size_t)out_len);
         dst->output_size = out_len;
      }
      if (src->inputs != NULL && in_len > 0)
      {
         memcpy(dst->inputs, src->inputs, (size_t)in_len);
         dst->input_size = in_len;
      }
   }

   LeaveCriticalSection(&G_core.state_lock);
}

static void recover_or_report_slaves(ecx_contextt *context)
{
   ec_groupt *grp = &context->grouplist[0];
   int i;

   grp->docheckstate = FALSE;
   ecx_readstate(context);

   for (i = 1; i <= context->slavecount; ++i)
   {
      ec_slavet *slave = &context->slavelist[i];
      if (slave->state == EC_STATE_OPERATIONAL)
      {
         continue;
      }

      grp->docheckstate = TRUE;
      if (slave->state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
      {
         slave->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
         ecx_writestate(context, (uint16)i);
      }
      else if (slave->state == EC_STATE_SAFE_OP)
      {
         slave->state = EC_STATE_OPERATIONAL;
         ecx_writestate(context, (uint16)i);
      }
      else if (slave->state > EC_STATE_NONE)
      {
         (void)ecx_reconfig_slave(context, (uint16)i, EC_TIMEOUTRET);
      }
      else if (!slave->islost)
      {
         ecx_statecheck(context, (uint16)i, EC_STATE_OPERATIONAL,
                        EC_TIMEOUTRET);
         if (slave->state == EC_STATE_NONE)
         {
            slave->islost = TRUE;
         }
      }
      else
      {
         (void)ecx_recover_slave(context, (uint16)i, EC_TIMEOUTRET);
      }
   }
}

static DWORD WINAPI worker_proc(LPVOID arg)
{
   (void)arg;

   ecx_contextt *context = &G_core.context;
   ec_groupt *grp;
   int expected_wkc = 0;
   int cycle_count = 0;
   int state_poll = 0;
   double avg_us = 0.0;

   InterlockedExchange(&G_core.worker_running, 1);
   set_state_text("Initializing", NULL);
   log_message(1, "EtherCAT worker started");

   memset(context, 0, sizeof(*context));

   EnterCriticalSection(&G_core.bus_lock);
   if (!ecx_init(context, G_core.adapter_name))
   {
      LeaveCriticalSection(&G_core.bus_lock);
      set_state_text("Disconnected", "No socket connection. Check Npcap/admin rights.");
      log_message(3, "ecx_init failed");
      InterlockedExchange(&G_core.opened, 0);
      InterlockedExchange(&G_core.worker_running, 0);
      return 0;
   }

   set_state_text("Scanning", NULL);
   if (ecx_config_init(context) <= 0)
   {
      ecx_close(context);
      LeaveCriticalSection(&G_core.bus_lock);
      set_state_text("Disconnected", "No EtherCAT slaves found.");
      log_message(3, "No EtherCAT slaves found");
      InterlockedExchange(&G_core.opened, 0);
      InterlockedExchange(&G_core.worker_running, 0);
      return 0;
   }

   grp = &context->grouplist[0];
   ecx_config_map_group(context, G_core.iomap, 0);
   ecx_configdc(context);
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   ecx_send_processdata(context);
   (void)ecx_receive_processdata(context, EC_TIMEOUTRET);
   expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
   update_slave_snapshot_locked(context);

   if (G_core.request_operational)
   {
      int i;
      context->slavelist[0].state = EC_STATE_OPERATIONAL;
      ecx_writestate(context, 0);
      for (i = 0; i < 40; ++i)
      {
         ecx_send_processdata(context);
         (void)ecx_receive_processdata(context, EC_TIMEOUTRET);
         ecx_statecheck(context, 0, EC_STATE_OPERATIONAL,
                        EC_TIMEOUTSTATE / 20);
         if (context->slavelist[0].state == EC_STATE_OPERATIONAL)
         {
            break;
         }
         osal_usleep(50000);
      }
   }

   ecx_readstate(context);
   update_slave_snapshot_locked(context);

   EnterCriticalSection(&G_core.state_lock);
   G_core.status.connected = 1;
   G_core.status.operational =
      (context->slavelist[0].state == EC_STATE_OPERATIONAL) ? 1 : 0;
   G_core.status.expected_wkc = expected_wkc;
   G_core.status.period_us = G_core.period_us;
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             G_core.status.operational ? "Operational" : "Safe-Operational");
   LeaveCriticalSection(&G_core.state_lock);

   LeaveCriticalSection(&G_core.bus_lock);
   log_message(1, "EtherCAT bus configured");

   while (InterlockedCompareExchange(&G_core.stop_flag, 0, 0) == 0)
   {
      LARGE_INTEGER start;
      LARGE_INTEGER end;
      int cycle_us;
      int sleep_us;
      int wkc;

      EnterCriticalSection(&G_core.bus_lock);
      QueryPerformanceCounter(&start);
      ecx_send_processdata(context);
      wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
      QueryPerformanceCounter(&end);

      cycle_us = elapsed_us(start, end);
      ++cycle_count;
      avg_us += ((double)cycle_us - avg_us) / (double)cycle_count;

      if (wkc < expected_wkc && expected_wkc > 0)
      {
         recover_or_report_slaves(context);
      }
      else if (++state_poll >= 50)
      {
         state_poll = 0;
         ecx_readstate(context);
      }

      update_slave_snapshot_locked(context);
      LeaveCriticalSection(&G_core.bus_lock);

      EnterCriticalSection(&G_core.state_lock);
      G_core.status.connected = 1;
      G_core.status.last_wkc = wkc;
      G_core.status.total_cycles = cycle_count;
      G_core.status.cycle_us = cycle_us;
      G_core.status.dc_time_ns = (long long)context->DCtime;
      if (G_core.status.min_cycle_us == 0 || cycle_us < G_core.status.min_cycle_us)
      {
         G_core.status.min_cycle_us = cycle_us;
      }
      if (cycle_us > G_core.status.max_cycle_us)
      {
         G_core.status.max_cycle_us = cycle_us;
      }
      G_core.status.avg_cycle_us = avg_us;
      if (wkc < expected_wkc && expected_wkc > 0)
      {
         ++G_core.status.wkc_errors;
         ++G_core.status.state_errors;
      }
      LeaveCriticalSection(&G_core.state_lock);

      sleep_us = G_core.period_us - cycle_us;
      if (sleep_us > 0)
      {
         osal_usleep((uint32)sleep_us);
      }
      else
      {
         Sleep(0);
      }
   }

   EnterCriticalSection(&G_core.bus_lock);
   if (context->slavecount > 0)
   {
      context->slavelist[0].state = EC_STATE_INIT;
      ecx_writestate(context, 0);
   }
   ecx_close(context);
   LeaveCriticalSection(&G_core.bus_lock);

   EnterCriticalSection(&G_core.state_lock);
   G_core.status.connected = 0;
   G_core.status.operational = 0;
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             "Disconnected");
   LeaveCriticalSection(&G_core.state_lock);

   InterlockedExchange(&G_core.opened, 0);
   InterlockedExchange(&G_core.worker_running, 0);
   log_message(1, "EtherCAT worker stopped");
   return 0;
}

int ECAT_GetVersion(char *buffer, int buffer_size)
{
   if (buffer == NULL || buffer_size <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   safe_copy(buffer, (size_t)buffer_size, ECAT_DLL_VERSION);
   return ECAT_OK;
}

const char *ECAT_ErrorToString(int error_code)
{
   switch (error_code)
   {
   case ECAT_OK:
      return "OK";
   case ECAT_ERROR:
      return "General EtherCAT error";
   case ECAT_INVALID_ARGUMENT:
      return "Invalid argument";
   case ECAT_BUSY:
      return "EtherCAT master is busy";
   case ECAT_NOT_OPEN:
      return "EtherCAT master is not open";
   case ECAT_TIMEOUT:
      return "Operation timed out";
   default:
      return "Unknown error";
   }
}

int ECAT_GetLastError(char *buffer, int buffer_size)
{
   ensure_initialized();
   if (buffer == NULL || buffer_size <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   EnterCriticalSection(&G_core.state_lock);
   safe_copy(buffer, (size_t)buffer_size,
             G_core.status.last_error[0] ? G_core.status.last_error : "No error");
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_SetLogCallback(ECAT_LogCallback callback)
{
   ensure_initialized();
   EnterCriticalSection(&G_core.state_lock);
   G_core.log_callback = callback;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_SetBackend(int backend_type)
{
   ensure_initialized();
   if (backend_type != ECAT_BACKEND_WINDOWS_DEBUG &&
       backend_type != ECAT_BACKEND_LINUX_RT)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (ECAT_IsOpen())
   {
      return ECAT_BUSY;
   }

   EnterCriticalSection(&G_core.state_lock);
   G_core.backend_type = backend_type;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_GetBackend(int *backend_type)
{
   ensure_initialized();
   if (backend_type == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   EnterCriticalSection(&G_core.state_lock);
   *backend_type = G_core.backend_type;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_SetLinuxRtEndpoint(const char *host, int port)
{
   ensure_initialized();
   if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (ECAT_IsOpen())
   {
      return ECAT_BUSY;
   }

   EnterCriticalSection(&G_core.rt_lock);
   safe_copy(G_core.linux_rt_host, sizeof(G_core.linux_rt_host), host);
   G_core.linux_rt_port = port;
   LeaveCriticalSection(&G_core.rt_lock);
   return ECAT_OK;
}

int ECAT_GetLinuxRtEndpoint(char *host, int host_size, int *port)
{
   ensure_initialized();
   if (host == NULL || host_size <= 0 || port == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   EnterCriticalSection(&G_core.rt_lock);
   safe_copy(host, (size_t)host_size, G_core.linux_rt_host);
   *port = G_core.linux_rt_port;
   LeaveCriticalSection(&G_core.rt_lock);
   return ECAT_OK;
}

int ECAT_ListAdapters(ECAT_AdapterInfo *adapters, int max_count,
                      int *actual_count)
{
   ec_adaptert *head;
   ec_adaptert *adapter;
   int count = 0;

   ensure_initialized();
   if (actual_count != NULL)
   {
      *actual_count = 0;
   }
   if (max_count < 0 || (max_count > 0 && adapters == NULL))
   {
      return ECAT_INVALID_ARGUMENT;
   }

   if (is_linux_rt_backend())
   {
      if (max_count > 0)
      {
         safe_copy(adapters[0].name, sizeof(adapters[0].name),
                   "Linux RT Controller");
         (void)snprintf(adapters[0].description,
                        sizeof(adapters[0].description),
                        "%s:%d", G_core.linux_rt_host,
                        G_core.linux_rt_port);
      }
      if (actual_count != NULL)
      {
         *actual_count = 1;
      }
      return ECAT_OK;
   }

   head = adapter = ec_find_adapters();
   while (adapter != NULL)
   {
      if (count < max_count)
      {
         safe_copy(adapters[count].name, sizeof(adapters[count].name),
                   adapter->name);
         safe_copy(adapters[count].description,
                   sizeof(adapters[count].description), adapter->desc);
      }
      ++count;
      adapter = adapter->next;
   }
   ec_free_adapters(head);

   if (actual_count != NULL)
   {
      *actual_count = count;
   }
   return ECAT_OK;
}

int ECAT_Open(const char *adapter_name, const ECAT_OpenOptions *options)
{
   ensure_initialized();
   if (is_linux_rt_backend())
   {
      (void)adapter_name;
      if (InterlockedCompareExchange(&G_core.worker_running, 0, 0) != 0)
      {
         return ECAT_BUSY;
      }
      return rt_open_backend(options);
   }
   if (adapter_name == NULL || adapter_name[0] == '\0')
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (InterlockedCompareExchange(&G_core.worker_running, 0, 0) != 0)
   {
      return ECAT_BUSY;
   }

   if (G_core.worker != NULL)
   {
      CloseHandle(G_core.worker);
      G_core.worker = NULL;
   }

   EnterCriticalSection(&G_core.state_lock);
   memset(&G_core.status, 0, sizeof(G_core.status));
   memset(G_core.slaves, 0, sizeof(G_core.slaves));
   safe_copy(G_core.status.state_text, sizeof(G_core.status.state_text),
             "Starting");
   safe_copy(G_core.status.crc_status, sizeof(G_core.status.crc_status),
             "Ethernet FCS/CRC is not exposed by Npcap/WinPcap.");
   safe_copy(G_core.adapter_name, sizeof(G_core.adapter_name), adapter_name);
   G_core.request_operational =
      (options != NULL) ? options->request_operational : 0;
   G_core.period_us =
      (options != NULL && options->period_us > 0)
         ? options->period_us
         : ECAT_DEFAULT_PERIOD_US;
   G_core.status.period_us = G_core.period_us;
   LeaveCriticalSection(&G_core.state_lock);

   InterlockedExchange(&G_core.stop_flag, 0);
   InterlockedExchange(&G_core.opened, 1);
   G_core.worker = CreateThread(NULL, 0, worker_proc, NULL, 0, NULL);
   if (G_core.worker == NULL)
   {
      InterlockedExchange(&G_core.opened, 0);
      set_state_text("Disconnected", "CreateThread failed.");
      return ECAT_ERROR;
   }
   return ECAT_OK;
}

int ECAT_Close(void)
{
   ensure_initialized();
   if (is_linux_rt_backend())
   {
      return rt_close_backend();
   }
   InterlockedExchange(&G_core.stop_flag, 1);
   if (G_core.worker != NULL)
   {
      DWORD wait_result = WaitForSingleObject(G_core.worker, 5000);
      CloseHandle(G_core.worker);
      G_core.worker = NULL;
      if (wait_result == WAIT_TIMEOUT)
      {
         InterlockedExchange(&G_core.worker_running, 0);
         InterlockedExchange(&G_core.opened, 0);
         return ECAT_TIMEOUT;
      }
   }
   return ECAT_OK;
}

int ECAT_IsOpen(void)
{
   ensure_initialized();
   return InterlockedCompareExchange(&G_core.opened, 0, 0) != 0 ? 1 : 0;
}

int ECAT_ResetStatistics(void)
{
   ensure_initialized();
   if (is_linux_rt_backend())
   {
      ECAT_NetCommand command;
      int result;
      memset(&command, 0, sizeof(command));
      command.command = ECAT_NET_CMD_RESET_STATISTICS;
      EnterCriticalSection(&G_core.rt_lock);
      result = rt_send_command_locked(&command);
      LeaveCriticalSection(&G_core.rt_lock);
      return result;
   }
   EnterCriticalSection(&G_core.state_lock);
   G_core.status.total_cycles = 0;
   G_core.status.wkc_errors = 0;
   G_core.status.state_errors = 0;
   G_core.status.cycle_us = 0;
   G_core.status.min_cycle_us = 0;
   G_core.status.max_cycle_us = 0;
   G_core.status.avg_cycle_us = 0.0;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_GetRuntimeStatus(ECAT_RuntimeStatus *status)
{
   ensure_initialized();
   if (status == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend() && ECAT_IsOpen())
   {
      (void)rt_poll_status();
   }
   EnterCriticalSection(&G_core.state_lock);
   *status = G_core.status;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_GetSlaveInfo(int slave_index, ECAT_SlaveInfo *info)
{
   ensure_initialized();
   if (info == NULL || slave_index <= 0 || slave_index >= EC_MAXSLAVE)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   EnterCriticalSection(&G_core.state_lock);
   if (slave_index > G_core.status.slave_count)
   {
      LeaveCriticalSection(&G_core.state_lock);
      return ECAT_INVALID_ARGUMENT;
   }
   *info = G_core.slaves[slave_index - 1].info;
   LeaveCriticalSection(&G_core.state_lock);
   return ECAT_OK;
}

int ECAT_GetPdoSnapshot(int slave_index, unsigned char *outputs,
                        int output_capacity, int *output_size,
                        unsigned char *inputs, int input_capacity,
                        int *input_size)
{
   InternalSlaveSnapshot snapshot;

   ensure_initialized();
   if (slave_index <= 0 || slave_index >= EC_MAXSLAVE)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   memset(&snapshot, 0, sizeof(snapshot));
   EnterCriticalSection(&G_core.state_lock);
   if (slave_index > G_core.status.slave_count)
   {
      LeaveCriticalSection(&G_core.state_lock);
      return ECAT_INVALID_ARGUMENT;
   }
   snapshot = G_core.slaves[slave_index - 1];
   LeaveCriticalSection(&G_core.state_lock);

   if (output_size != NULL)
   {
      *output_size = snapshot.output_size;
   }
   if (input_size != NULL)
   {
      *input_size = snapshot.input_size;
   }
   if (outputs != NULL && output_capacity > 0)
   {
      int copy = snapshot.output_size < output_capacity
                    ? snapshot.output_size
                    : output_capacity;
      memcpy(outputs, snapshot.outputs, (size_t)copy);
      if (output_size != NULL)
      {
         *output_size = copy;
      }
   }
   if (inputs != NULL && input_capacity > 0)
   {
      int copy = snapshot.input_size < input_capacity
                    ? snapshot.input_size
                    : input_capacity;
      memcpy(inputs, snapshot.inputs, (size_t)copy);
      if (input_size != NULL)
      {
         *input_size = copy;
      }
   }
   return ECAT_OK;
}

int ECAT_ReadSdo(int slave_index, unsigned short index, unsigned char subindex,
                 unsigned char *data, int data_capacity, int *data_size)
{
   int size;
   int wkc;

   ensure_initialized();
   if (is_linux_rt_backend())
   {
      return rt_read_sdo(slave_index, index, subindex, data, data_capacity,
                         data_size);
   }
   if (data == NULL || data_capacity <= 0 || slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (InterlockedCompareExchange(&G_core.worker_running, 0, 0) == 0)
   {
      return ECAT_NOT_OPEN;
   }

   size = data_capacity;
   memset(data, 0, (size_t)data_capacity);

   EnterCriticalSection(&G_core.bus_lock);
   if (slave_index > G_core.context.slavecount)
   {
      LeaveCriticalSection(&G_core.bus_lock);
      return ECAT_INVALID_ARGUMENT;
   }
   wkc = ecx_SDOread(&G_core.context, (uint16)slave_index, index, subindex,
                     FALSE, &size, data, EC_TIMEOUTRXM);
   LeaveCriticalSection(&G_core.bus_lock);

   if (data_size != NULL)
   {
      *data_size = (wkc > 0) ? size : 0;
   }
   return (wkc > 0) ? ECAT_OK : ECAT_ERROR;
}

int ECAT_WriteSdo(int slave_index, unsigned short index, unsigned char subindex,
                  const unsigned char *data, int data_size)
{
   int wkc;

   ensure_initialized();
   if (is_linux_rt_backend())
   {
      return rt_write_sdo(slave_index, index, subindex, data, data_size);
   }
   if (data == NULL || data_size <= 0 || slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (InterlockedCompareExchange(&G_core.worker_running, 0, 0) == 0)
   {
      return ECAT_NOT_OPEN;
   }

   EnterCriticalSection(&G_core.bus_lock);
   if (slave_index > G_core.context.slavecount)
   {
      LeaveCriticalSection(&G_core.bus_lock);
      return ECAT_INVALID_ARGUMENT;
   }
   wkc = ecx_SDOwrite(&G_core.context, (uint16)slave_index, index, subindex,
                      FALSE, data_size, data, EC_TIMEOUTRXM);
   LeaveCriticalSection(&G_core.bus_lock);

   return (wkc > 0) ? ECAT_OK : ECAT_ERROR;
}

static int read_sdo_u8(int slave_index, unsigned short index,
                       unsigned char subindex, unsigned char *value)
{
   unsigned char data[1];
   int size = 0;
   int result;

   if (value == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   result = ECAT_ReadSdo(slave_index, index, subindex, data, sizeof(data),
                         &size);
   if (result != ECAT_OK || size < (int)sizeof(data))
   {
      return result != ECAT_OK ? result : ECAT_ERROR;
   }
   *value = data[0];
   return ECAT_OK;
}

static int read_sdo_u16(int slave_index, unsigned short index,
                        unsigned char subindex, unsigned short *value)
{
   unsigned char data[2];
   int size = 0;
   int result;

   if (value == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   result = ECAT_ReadSdo(slave_index, index, subindex, data, sizeof(data),
                         &size);
   if (result != ECAT_OK || size < (int)sizeof(data))
   {
      return result != ECAT_OK ? result : ECAT_ERROR;
   }
   *value = (unsigned short)(data[0] | ((unsigned short)data[1] << 8));
   return ECAT_OK;
}

static int read_sdo_i32(int slave_index, unsigned short index,
                        unsigned char subindex, int *value)
{
   unsigned char data[4];
   unsigned int raw;
   int size = 0;
   int result;

   if (value == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   result = ECAT_ReadSdo(slave_index, index, subindex, data, sizeof(data),
                         &size);
   if (result != ECAT_OK || size < (int)sizeof(data))
   {
      return result != ECAT_OK ? result : ECAT_ERROR;
   }
   raw = ((unsigned int)data[0]) |
         ((unsigned int)data[1] << 8) |
         ((unsigned int)data[2] << 16) |
         ((unsigned int)data[3] << 24);
   *value = (int)raw;
   return ECAT_OK;
}

static int write_sdo_u8(int slave_index, unsigned short index,
                        unsigned char subindex, unsigned char value)
{
   return ECAT_WriteSdo(slave_index, index, subindex, &value,
                        (int)sizeof(value));
}

static int write_sdo_i8(int slave_index, unsigned short index,
                        unsigned char subindex, signed char value)
{
   return ECAT_WriteSdo(slave_index, index, subindex,
                        (const unsigned char *)&value, (int)sizeof(value));
}

static int write_sdo_u16(int slave_index, unsigned short index,
                         unsigned char subindex, unsigned short value)
{
   unsigned char data[2];

   data[0] = (unsigned char)(value & 0xffU);
   data[1] = (unsigned char)((value >> 8) & 0xffU);
   return ECAT_WriteSdo(slave_index, index, subindex, data, sizeof(data));
}

static int write_sdo_u32(int slave_index, unsigned short index,
                         unsigned char subindex, unsigned int value)
{
   unsigned char data[4];

   data[0] = (unsigned char)(value & 0xffU);
   data[1] = (unsigned char)((value >> 8) & 0xffU);
   data[2] = (unsigned char)((value >> 16) & 0xffU);
   data[3] = (unsigned char)((value >> 24) & 0xffU);
   return ECAT_WriteSdo(slave_index, index, subindex, data, sizeof(data));
}

static int write_sdo_i32(int slave_index, unsigned short index,
                         unsigned char subindex, int value)
{
   return write_sdo_u32(slave_index, index, subindex, (unsigned int)value);
}

static int servo_write_controlword(int slave_index, unsigned short controlword)
{
   return write_sdo_u16(slave_index, 0x6040, 0, controlword);
}

static int return_on_error(int result)
{
   return result == ECAT_OK ? 0 : result;
}

int ECAT_ServoGetStatus(int slave_index, ECAT_ServoStatus *status)
{
   unsigned char mode = 0;
   int result;

   ensure_initialized();
   if (status == NULL || slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   memset(status, 0, sizeof(*status));
   result = read_sdo_u16(slave_index, 0x6041, 0, &status->statusword);
   if (result != ECAT_OK)
   {
      return result;
   }

   (void)read_sdo_u16(slave_index, 0x6040, 0, &status->controlword);
   if (read_sdo_u8(slave_index, 0x6061, 0, &mode) == ECAT_OK)
   {
      status->mode_display = (signed char)mode;
   }
   (void)read_sdo_i32(slave_index, 0x6064, 0, &status->actual_position);
   (void)read_sdo_i32(slave_index, 0x606C, 0, &status->actual_velocity);
   (void)read_sdo_u16(slave_index, 0x603F, 0, &status->error_code);

   status->target_reached = (status->statusword & 0x0400U) ? 1 : 0;
   status->fault = (status->statusword & 0x0008U) ? 1 : 0;
   status->warning = (status->statusword & 0x0080U) ? 1 : 0;
   safe_copy(status->cia402_state, sizeof(status->cia402_state),
             ECAT_Cia402StateName(status->statusword));
   return ECAT_OK;
}

int ECAT_ServoFaultReset(int slave_index)
{
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_FAULT_RESET,
                                    slave_index, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   }
   result = servo_write_controlword(slave_index, 0x0080);
   if (result != ECAT_OK)
   {
      return result;
   }
   Sleep(50);
   return servo_write_controlword(slave_index, 0x0006);
}

int ECAT_ServoEnable(int slave_index)
{
   ECAT_ServoStatus status;
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_ENABLE,
                                    slave_index, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   }

   if (ECAT_ServoGetStatus(slave_index, &status) == ECAT_OK && status.fault)
   {
      result = ECAT_ServoFaultReset(slave_index);
      if (result != ECAT_OK)
      {
         return result;
      }
      Sleep(50);
   }

   result = return_on_error(servo_write_controlword(slave_index, 0x0006));
   if (result != 0)
   {
      return result;
   }
   Sleep(20);
   result = return_on_error(servo_write_controlword(slave_index, 0x0007));
   if (result != 0)
   {
      return result;
   }
   Sleep(20);
   return servo_write_controlword(slave_index, 0x000F);
}

int ECAT_ServoDisable(int slave_index)
{
   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_DISABLE,
                                    slave_index, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   }
   return servo_write_controlword(slave_index, 0x0006);
}

int ECAT_ServoSetMode(int slave_index, signed char mode)
{
   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_SET_MODE,
                                    slave_index, 0, 0, 0, 0, 0, mode, 0, 0, 0);
   }
   return write_sdo_i8(slave_index, 0x6060, 0, mode);
}

int ECAT_ServoMoveAbs(int slave_index, int target_position,
                      unsigned int velocity,
                      unsigned int acceleration,
                      unsigned int deceleration)
{
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_MOVE_ABS,
                                    slave_index, target_position, 0, velocity,
                                    acceleration, deceleration, 0, 0, 0, 0);
   }

   result = return_on_error(
      ECAT_ServoSetMode(slave_index, ECAT_CIA402_MODE_PROFILE_POSITION));
   if (result != 0)
   {
      return result;
   }
   if (velocity > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6081, 0,
                                            velocity));
      if (result != 0)
      {
         return result;
      }
   }
   if (acceleration > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6083, 0,
                                            acceleration));
      if (result != 0)
      {
         return result;
      }
   }
   if (deceleration > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6084, 0,
                                            deceleration));
      if (result != 0)
      {
         return result;
      }
   }

   result = return_on_error(write_sdo_i32(slave_index, 0x607A, 0,
                                         target_position));
   if (result != 0)
   {
      return result;
   }
   result = return_on_error(ECAT_ServoEnable(slave_index));
   if (result != 0)
   {
      return result;
   }
   result = return_on_error(servo_write_controlword(slave_index, 0x003F));
   if (result != 0)
   {
      return result;
   }
   Sleep(10);
   return servo_write_controlword(slave_index, 0x000F);
}

int ECAT_ServoMoveRel(int slave_index, int distance,
                      unsigned int velocity,
                      unsigned int acceleration,
                      unsigned int deceleration)
{
   int current_position;
   long long target_position;
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_MOVE_REL,
                                    slave_index, distance, 0, velocity,
                                    acceleration, deceleration, 0, 0, 0, 0);
   }

   result = ECAT_ServoGetPosition(slave_index, &current_position);
   if (result != ECAT_OK)
   {
      return result;
   }
   target_position = (long long)current_position + (long long)distance;
   if (target_position < INT_MIN || target_position > INT_MAX)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   return ECAT_ServoMoveAbs(slave_index, (int)target_position, velocity,
                            acceleration, deceleration);
}

int ECAT_ServoMoveVel(int slave_index, int target_velocity,
                      unsigned int acceleration,
                      unsigned int deceleration)
{
   return ECAT_ServoJog(slave_index, target_velocity, acceleration,
                        deceleration);
}

int ECAT_ServoJog(int slave_index, int target_velocity,
                  unsigned int acceleration,
                  unsigned int deceleration)
{
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_JOG,
                                    slave_index, 0, target_velocity, 0,
                                    acceleration, deceleration, 0, 0, 0, 0);
   }

   result = return_on_error(
      ECAT_ServoSetMode(slave_index, ECAT_CIA402_MODE_PROFILE_VELOCITY));
   if (result != 0)
   {
      return result;
   }
   if (acceleration > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6083, 0,
                                            acceleration));
      if (result != 0)
      {
         return result;
      }
   }
   if (deceleration > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6084, 0,
                                            deceleration));
      if (result != 0)
      {
         return result;
      }
   }
   result = return_on_error(write_sdo_i32(slave_index, 0x60FF, 0,
                                         target_velocity));
   if (result != 0)
   {
      return result;
   }
   return ECAT_ServoEnable(slave_index);
}

int ECAT_ServoHome(int slave_index, signed char homing_method,
                   unsigned int search_speed,
                   unsigned int latch_speed,
                   unsigned int acceleration)
{
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_HOME,
                                    slave_index, 0, 0, 0, acceleration, 0, 0,
                                    homing_method, search_speed, latch_speed);
   }

   result = return_on_error(
      ECAT_ServoSetMode(slave_index, ECAT_CIA402_MODE_HOMING));
   if (result != 0)
   {
      return result;
   }
   result = return_on_error(write_sdo_i8(slave_index, 0x6098, 0,
                                        homing_method));
   if (result != 0)
   {
      return result;
   }
   if (search_speed > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6099, 1,
                                            search_speed));
      if (result != 0)
      {
         return result;
      }
   }
   if (latch_speed > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x6099, 2,
                                            latch_speed));
      if (result != 0)
      {
         return result;
      }
   }
   if (acceleration > 0)
   {
      result = return_on_error(write_sdo_u32(slave_index, 0x609A, 0,
                                            acceleration));
      if (result != 0)
      {
         return result;
      }
   }
   result = return_on_error(ECAT_ServoEnable(slave_index));
   if (result != 0)
   {
      return result;
   }
   return servo_write_controlword(slave_index, 0x001F);
}

int ECAT_ServoStop(int slave_index)
{
   int result;

   ensure_initialized();
   if (slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   if (is_linux_rt_backend())
   {
      return rt_send_motion_command(ECAT_NET_CMD_SERVO_STOP,
                                    slave_index, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   }

   (void)write_sdo_i32(slave_index, 0x60FF, 0, 0);
   result = servo_write_controlword(slave_index, 0x010F);
   if (result != ECAT_OK)
   {
      return result;
   }
   return ECAT_OK;
}

int ECAT_ServoGetPosition(int slave_index, int *position)
{
   ensure_initialized();
   if (position == NULL || slave_index <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   return read_sdo_i32(slave_index, 0x6064, 0, position);
}

int ECAT_LMS_MoveAbs(int slave_index, int target_position,
                     unsigned int velocity)
{
   return ECAT_ServoMoveAbs(slave_index, target_position, velocity, 0, 0);
}

int ECAT_LMS_MoveVel(int slave_index, int target_velocity)
{
   return ECAT_ServoJog(slave_index, target_velocity, 0, 0);
}

int ECAT_LMS_Stop(int slave_index)
{
   return ECAT_ServoStop(slave_index);
}

int ECAT_LMS_GetMoverPosition(int slave_index, int *position)
{
   return ECAT_ServoGetPosition(slave_index, position);
}

int ECAT_DbSetRoot(const char *root_dir)
{
   int result;

   ensure_initialized();
   if (root_dir == NULL || root_dir[0] == '\0')
   {
      return ECAT_INVALID_ARGUMENT;
   }

   EnterCriticalSection(&G_core.db_lock);
   safe_copy(G_core.db_root, sizeof(G_core.db_root), root_dir);
   result = db_reload_locked();
   LeaveCriticalSection(&G_core.db_lock);
   return result;
}

int ECAT_DbGetRoot(char *buffer, int buffer_size)
{
   ensure_initialized();
   if (buffer == NULL || buffer_size <= 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   EnterCriticalSection(&G_core.db_lock);
   safe_copy(buffer, (size_t)buffer_size, G_core.db_root);
   LeaveCriticalSection(&G_core.db_lock);
   return ECAT_OK;
}

int ECAT_DbReload(void)
{
   int result;

   ensure_initialized();
   EnterCriticalSection(&G_core.db_lock);
   result = db_reload_locked();
   LeaveCriticalSection(&G_core.db_lock);
   return result;
}

int ECAT_DbImportXml(const char *xml_path, ECAT_DbEntry *imported)
{
   char *xml_text;
   ECAT_DbEntry first_entry;
   char stored_path[ECAT_MAX_PATH_TEXT];
   int result;

   ensure_initialized();
   if (xml_path == NULL || xml_path[0] == '\0')
   {
      return ECAT_INVALID_ARGUMENT;
   }

   result = read_file_text(xml_path, &xml_text);
   if (result != ECAT_OK)
   {
      return result;
   }
   result = parse_xml_device(xml_text, &first_entry);
   if (result != ECAT_OK)
   {
      free(xml_text);
      return result;
   }

   EnterCriticalSection(&G_core.db_lock);
   result = ensure_db_dirs_locked();
   if (result == ECAT_OK)
   {
      build_xml_store_path_locked(&first_entry, xml_path, stored_path,
                                  sizeof(stored_path));
      if (lstrcmpiA(xml_path, stored_path) != 0 &&
          !CopyFileA(xml_path, stored_path, FALSE))
      {
         result = ECAT_ERROR;
      }
   }
   if (result == ECAT_OK)
   {
      result = db_reload_locked();
   }
   if (result == ECAT_OK)
   {
      result = db_upsert_xml_entries_locked(xml_text, stored_path,
                                            &first_entry);
   }
   if (result == ECAT_OK)
   {
      result = db_save_locked();
   }
   if (result == ECAT_OK && imported != NULL)
   {
      *imported = first_entry;
   }
   LeaveCriticalSection(&G_core.db_lock);
   free(xml_text);

   if (result == ECAT_OK)
   {
      char message[512];
      (void)snprintf(message, sizeof(message),
                     "XML imported: %s (VID=0x%08X PID=0x%08X REV=0x%08X)",
                     first_entry.name, first_entry.vendor_id,
                     first_entry.product_code, first_entry.revision);
      log_message(1, message);
   }
   return result;
}

int ECAT_DbGetCount(int *count)
{
   ensure_initialized();
   if (count == NULL)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   EnterCriticalSection(&G_core.db_lock);
   *count = G_core.db_count;
   LeaveCriticalSection(&G_core.db_lock);
   return ECAT_OK;
}

int ECAT_DbGetEntry(int index, ECAT_DbEntry *entry)
{
   ensure_initialized();
   if (entry == NULL || index < 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }

   EnterCriticalSection(&G_core.db_lock);
   if (index >= G_core.db_count)
   {
      LeaveCriticalSection(&G_core.db_lock);
      return ECAT_INVALID_ARGUMENT;
   }
   *entry = G_core.db_entries[index];
   LeaveCriticalSection(&G_core.db_lock);
   return ECAT_OK;
}

int ECAT_DbFindDevice(unsigned int vendor_id,
                      unsigned int product_code,
                      unsigned int revision,
                      ECAT_DbEntry *entry)
{
   ensure_initialized();
   if (vendor_id == 0 || product_code == 0)
   {
      return ECAT_INVALID_ARGUMENT;
   }
   return db_find_match_threadsafe(vendor_id, product_code, revision, entry);
}
