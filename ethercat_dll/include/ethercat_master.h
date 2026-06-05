#pragma once
#ifndef ETHERCAT_MASTER_H
#define ETHERCAT_MASTER_H

#ifdef _WIN32
#ifdef ETHERCAT_MASTER_EXPORTS
#define ECAT_API __declspec(dllexport)
#else
#define ECAT_API __declspec(dllimport)
#endif
#else
#define ECAT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_OK 0
#define ECAT_ERROR -1
#define ECAT_INVALID_ARGUMENT -2
#define ECAT_BUSY -3
#define ECAT_NOT_OPEN -4
#define ECAT_TIMEOUT -5

#define ECAT_MAX_ADAPTER_NAME 128
#define ECAT_MAX_SLAVE_NAME 41
#define ECAT_MAX_PDO_COPY 256
#define ECAT_MAX_MESSAGE 256
#define ECAT_MAX_PATH_TEXT 260
#define ECAT_MAX_DB_ENTRIES 512

#define ECAT_CIA402_MODE_PROFILE_POSITION 1
#define ECAT_CIA402_MODE_PROFILE_VELOCITY 3
#define ECAT_CIA402_MODE_HOMING 6
#define ECAT_CIA402_MODE_CSP 8
#define ECAT_CIA402_MODE_CSV 9
#define ECAT_CIA402_MODE_CST 10

#define ECAT_BACKEND_WINDOWS_DEBUG 0
#define ECAT_BACKEND_LINUX_RT 1

typedef void (*ECAT_LogCallback)(int level, const char *message);

typedef struct ECAT_AdapterInfo
{
   char name[ECAT_MAX_ADAPTER_NAME];
   char description[ECAT_MAX_ADAPTER_NAME];
} ECAT_AdapterInfo;

typedef struct ECAT_OpenOptions
{
   int request_operational;
   int period_us;
} ECAT_OpenOptions;

typedef struct ECAT_RuntimeStatus
{
   int connected;
   int operational;
   int slave_count;
   int expected_wkc;
   int last_wkc;
   int total_cycles;
   int wkc_errors;
   int state_errors;
   int cycle_us;
   int min_cycle_us;
   int max_cycle_us;
   double avg_cycle_us;
   int period_us;
   long long dc_time_ns;
   char state_text[ECAT_MAX_MESSAGE];
   char last_error[ECAT_MAX_MESSAGE];
   char crc_status[ECAT_MAX_MESSAGE];
} ECAT_RuntimeStatus;

typedef struct ECAT_SlaveInfo
{
   int index;
   char name[ECAT_MAX_SLAVE_NAME];
   unsigned short state;
   unsigned short al_status;
   unsigned short config_address;
   unsigned short alias_address;
   unsigned int vendor_id;
   unsigned int product_code;
   unsigned int revision;
   unsigned int serial;
   unsigned short output_bits;
   unsigned int output_bytes;
   unsigned short input_bits;
   unsigned int input_bytes;
   unsigned short mailbox_write_bytes;
   unsigned short mailbox_read_bytes;
   unsigned short mailbox_protocols;
   int has_dc;
   int is_lost;
   int database_matched;
   char database_name[ECAT_MAX_MESSAGE];
   char database_xml[ECAT_MAX_PATH_TEXT];
} ECAT_SlaveInfo;

typedef struct ECAT_DbEntry
{
   unsigned int vendor_id;
   unsigned int product_code;
   unsigned int revision;
   char name[ECAT_MAX_MESSAGE];
   char xml_path[ECAT_MAX_PATH_TEXT];
   char xml_type[32];
   char imported_at[32];
} ECAT_DbEntry;

typedef struct ECAT_ServoStatus
{
   unsigned short statusword;
   unsigned short controlword;
   signed char mode_display;
   int actual_position;
   int actual_velocity;
   unsigned short error_code;
   int target_reached;
   int fault;
   int warning;
   char cia402_state[ECAT_MAX_MESSAGE];
} ECAT_ServoStatus;

ECAT_API int ECAT_GetVersion(char *buffer, int buffer_size);
ECAT_API const char *ECAT_ErrorToString(int error_code);
ECAT_API int ECAT_GetLastError(char *buffer, int buffer_size);
ECAT_API int ECAT_SetLogCallback(ECAT_LogCallback callback);
ECAT_API int ECAT_SetBackend(int backend_type);
ECAT_API int ECAT_GetBackend(int *backend_type);
ECAT_API int ECAT_SetLinuxRtEndpoint(const char *host, int port);
ECAT_API int ECAT_GetLinuxRtEndpoint(char *host, int host_size, int *port);
ECAT_API int ECAT_ListAdapters(ECAT_AdapterInfo *adapters, int max_count,
                               int *actual_count);
ECAT_API int ECAT_Open(const char *adapter_name,
                       const ECAT_OpenOptions *options);
ECAT_API int ECAT_Close(void);
ECAT_API int ECAT_IsOpen(void);
ECAT_API int ECAT_ResetStatistics(void);
ECAT_API int ECAT_GetRuntimeStatus(ECAT_RuntimeStatus *status);
ECAT_API int ECAT_GetSlaveInfo(int slave_index, ECAT_SlaveInfo *info);
ECAT_API int ECAT_GetPdoSnapshot(int slave_index, unsigned char *outputs,
                                 int output_capacity, int *output_size,
                                 unsigned char *inputs, int input_capacity,
                                 int *input_size);
ECAT_API int ECAT_ReadSdo(int slave_index, unsigned short index,
                          unsigned char subindex, unsigned char *data,
                          int data_capacity, int *data_size);
ECAT_API int ECAT_WriteSdo(int slave_index, unsigned short index,
                           unsigned char subindex, const unsigned char *data,
                           int data_size);
ECAT_API int ECAT_DbSetRoot(const char *root_dir);
ECAT_API int ECAT_DbGetRoot(char *buffer, int buffer_size);
ECAT_API int ECAT_DbReload(void);
ECAT_API int ECAT_DbImportXml(const char *xml_path, ECAT_DbEntry *imported);
ECAT_API int ECAT_DbGetCount(int *count);
ECAT_API int ECAT_DbGetEntry(int index, ECAT_DbEntry *entry);
ECAT_API int ECAT_DbFindDevice(unsigned int vendor_id,
                               unsigned int product_code,
                               unsigned int revision,
                               ECAT_DbEntry *entry);
ECAT_API int ECAT_ServoGetStatus(int slave_index, ECAT_ServoStatus *status);
ECAT_API int ECAT_ServoFaultReset(int slave_index);
ECAT_API int ECAT_ServoEnable(int slave_index);
ECAT_API int ECAT_ServoDisable(int slave_index);
ECAT_API int ECAT_ServoSetMode(int slave_index, signed char mode);
ECAT_API int ECAT_ServoMoveAbs(int slave_index, int target_position,
                               unsigned int velocity,
                               unsigned int acceleration,
                               unsigned int deceleration);
ECAT_API int ECAT_ServoJog(int slave_index, int target_velocity,
                           unsigned int acceleration,
                           unsigned int deceleration);
ECAT_API int ECAT_ServoHome(int slave_index, signed char homing_method,
                            unsigned int search_speed,
                            unsigned int latch_speed,
                            unsigned int acceleration);
ECAT_API int ECAT_ServoStop(int slave_index);
ECAT_API int ECAT_LMS_MoveAbs(int slave_index, int target_position,
                              unsigned int velocity);
ECAT_API int ECAT_LMS_MoveVel(int slave_index, int target_velocity);
ECAT_API int ECAT_LMS_Stop(int slave_index);
ECAT_API int ECAT_LMS_GetMoverPosition(int slave_index, int *position);
ECAT_API const char *ECAT_StateName(unsigned short state);
ECAT_API const char *ECAT_Cia402StateName(unsigned short statusword);

#ifdef __cplusplus
}
#endif

#endif
