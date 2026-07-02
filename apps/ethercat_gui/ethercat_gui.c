/*
 * EtherCAT diagnostics GUI example.
 *
 * This application is intentionally a DLL consumer. It uses only the public
 * ethercat_master.h API so it can be shipped as a sample beside the
 * distributable DLL/LIB/H package.
 */

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ethercat_master.h"
#include "resource.h"

#ifdef _MSC_VER
#pragma comment(linker,                                                         \
                "\"/manifestdependency:type='win32' "                          \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "   \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")
#endif

#define APP_TITLE "SOEM EtherCAT Master Monitor"
#define UPDATE_TIMER_ID 1
#define UPDATE_TIMER_MS 200
#define DEFAULT_PERIOD_US 1000
#define MAX_GUI_LOG 65536
#define GUI_MAX_SLAVES 64
#define GUI_MAX_COE_ENTRIES 256
#define GUI_COE_AUTO_UPDATE_MS 2000
#define WM_GUI_STATUS_READY (WM_APP + 1)
#define WM_GUI_COE_READY (WM_APP + 2)

typedef struct GuiSlaveSnapshot
{
   int valid;
   ECAT_SlaveInfo info;
   unsigned char outputs[ECAT_MAX_PDO_COPY];
   int output_size;
   unsigned char inputs[ECAT_MAX_PDO_COPY];
   int input_size;
} GuiSlaveSnapshot;

typedef struct GuiSnapshot
{
   int is_open;
   ECAT_RuntimeStatus runtime;
   GuiSlaveSnapshot slaves[GUI_MAX_SLAVES];
} GuiSnapshot;

typedef struct GuiCoeEntry
{
   unsigned short index;
   unsigned char subindex;
   char name[96];
   char flags[16];
   char value[192];
   int online;
   int size;
} GuiCoeEntry;

typedef struct GuiCoeSnapshot
{
   int slave;
   int result;
   int count;
   char detail[2048];
   GuiCoeEntry entries[GUI_MAX_COE_ENTRIES];
} GuiCoeSnapshot;

typedef struct MotionRepeatArgs
{
   int slave;
   int position1;
   int position2;
   unsigned int velocity;
   unsigned int acceleration;
   unsigned int deceleration;
   int delay_ms;
   int check_inpos;
} MotionRepeatArgs;

typedef struct CoeThreadArgs
{
   int slave;
   int show_offline;
} CoeThreadArgs;

typedef struct GuiState
{
   HWND hwnd;
   HWND backend;
   HWND rt_host;
   HWND rt_port;
   HWND adapter;
   HWND refresh;
   HWND connect;
   HWND disconnect;
   HWND opmode;
   HWND period;
   HWND summary;
   HWND slaves;
   HWND tabs;
   HWND status_list;
   HWND pdo_edit;
   HWND sdo_slave;
   HWND sdo_index;
   HWND sdo_sub;
   HWND sdo_size;
   HWND sdo_read;
   HWND sdo_result;
   HWND motion_slave;
   HWND motion_target;
   HWND motion_position2;
   HWND motion_velocity;
   HWND motion_accel;
   HWND motion_decel;
   HWND motion_home_method;
   HWND motion_profile;
   HWND motion_command_pos;
   HWND motion_actual_pos;
   HWND motion_op_status;
   HWND motion_command_vel;
   HWND motion_actual_vel;
   HWND motion_fault_reset;
   HWND motion_enable;
   HWND motion_disable;
   HWND motion_stop;
   HWND motion_jog_pos;
   HWND motion_jog_neg;
   HWND motion_move_abs;
   HWND motion_move_abs2;
   HWND motion_move_rel;
   HWND motion_home;
   HWND motion_repeat;
   HWND motion_delay;
   HWND motion_check_inpos;
   HWND motion_status;
   HWND coe_update;
   HWND coe_auto_update;
   HWND coe_single_update;
   HWND coe_show_offline;
   HWND coe_slave;
   HWND coe_list;
   HWND coe_detail;
   HWND stats_edit;
   HWND xml_import;
   HWND xml_reload;
   HWND xml_list;
   HWND xml_detail;
   HWND log_edit;
   HWND sdo_labels[4];
   HWND motion_labels[15];
   HWND coe_labels[1];
   HWND rt_labels[2];
   int active_tab;
   int selected_slave;
   CRITICAL_SECTION log_lock;
   CRITICAL_SECTION snapshot_lock;
   CRITICAL_SECTION coe_lock;
   char log_text[MAX_GUI_LOG];
   int log_len;
   HANDLE poll_thread;
   HANDLE repeat_thread;
   HANDLE coe_thread;
   volatile LONG poll_stop;
   volatile LONG update_pending;
   volatile LONG repeat_stop;
   volatile LONG repeat_running;
   volatile LONG coe_running;
   DWORD last_coe_auto_update_tick;
   GuiSnapshot snapshot;
   GuiCoeSnapshot coe_snapshot;
} GuiState;

static GuiState G_gui;

static INT_PTR CALLBACK MainDlgProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam);

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

static void append_log_line(const char *level, const char *message)
{
   SYSTEMTIME st;
   char line[1024];
   int written;

   GetLocalTime(&st);
   written = snprintf(line, sizeof(line),
                      "%02u:%02u:%02u.%03u [%s] %s\r\n",
                      (unsigned)st.wHour, (unsigned)st.wMinute,
                      (unsigned)st.wSecond, (unsigned)st.wMilliseconds,
                      level, message != NULL ? message : "");
   if (written <= 0)
   {
      return;
   }

   EnterCriticalSection(&G_gui.log_lock);
   if (G_gui.log_len + written >= MAX_GUI_LOG)
   {
      int keep = MAX_GUI_LOG / 2;
      memmove(G_gui.log_text, G_gui.log_text + G_gui.log_len - keep,
              (size_t)keep);
      G_gui.log_len = keep;
      G_gui.log_text[G_gui.log_len] = '\0';
   }
   memcpy(G_gui.log_text + G_gui.log_len, line, (size_t)written);
   G_gui.log_len += written;
   G_gui.log_text[G_gui.log_len] = '\0';
   LeaveCriticalSection(&G_gui.log_lock);
}

static void dll_log_callback(int level, const char *message)
{
   const char *label = "INFO";
   if (level >= 3)
   {
      label = "ERROR";
   }
   else if (level == 2)
   {
      label = "WARN";
   }
   append_log_line(label, message);
}

static unsigned short read_u16_le(const unsigned char *data)
{
   return (unsigned short)(data[0] | ((unsigned short)data[1] << 8));
}

static int read_i32_le(const unsigned char *data)
{
   unsigned int value = (unsigned int)data[0] |
                        ((unsigned int)data[1] << 8) |
                        ((unsigned int)data[2] << 16) |
                        ((unsigned int)data[3] << 24);
   return (int)(int32_t)value;
}

static void hex_dump(char *dst, size_t dst_size, const unsigned char *data,
                     int len)
{
   int offset = 0;
   size_t used = 0;

   if (dst_size == 0)
   {
      return;
   }
   dst[0] = '\0';
   if (data == NULL || len <= 0)
   {
      safe_copy(dst, dst_size, "(no mapped bytes)\r\n");
      return;
   }

   while (offset < len && used + 90 < dst_size)
   {
      int i;
      int row = len - offset;
      char ascii[17];
      if (row > 16)
      {
         row = 16;
      }

      used += (size_t)snprintf(dst + used, dst_size - used, "%04X  ", offset);
      for (i = 0; i < 16; ++i)
      {
         if (i < row)
         {
            used += (size_t)snprintf(dst + used, dst_size - used, "%02X ",
                                     data[offset + i]);
            ascii[i] = (data[offset + i] >= 32 && data[offset + i] <= 126)
                          ? (char)data[offset + i]
                          : '.';
         }
         else
         {
            used += (size_t)snprintf(dst + used, dst_size - used, "   ");
            ascii[i] = ' ';
         }
      }
      ascii[16] = '\0';
      used += (size_t)snprintf(dst + used, dst_size - used, " |%s|\r\n", ascii);
      offset += row;
   }
}

static void add_list_column(HWND list, int index, const char *title, int width)
{
   LVCOLUMNA col;
   memset(&col, 0, sizeof(col));
   col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
   col.pszText = (LPSTR)title;
   col.cx = width;
   col.iSubItem = index;
   ListView_InsertColumn(list, index, &col);
}

static void init_list_columns(void)
{
   ListView_SetExtendedListViewStyle(G_gui.slaves,
                                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
   add_list_column(G_gui.slaves, 0, "#", 42);
   add_list_column(G_gui.slaves, 1, "Slave Module", 155);
   add_list_column(G_gui.slaves, 2, "State", 95);
   add_list_column(G_gui.slaves, 3, "I/O", 80);

   ListView_SetExtendedListViewStyle(G_gui.status_list,
                                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
   add_list_column(G_gui.status_list, 0, "#", 42);
   add_list_column(G_gui.status_list, 1, "Name", 180);
   add_list_column(G_gui.status_list, 2, "State", 100);
   add_list_column(G_gui.status_list, 3, "AL", 70);
   add_list_column(G_gui.status_list, 4, "Output", 75);
   add_list_column(G_gui.status_list, 5, "Input", 75);
   add_list_column(G_gui.status_list, 6, "Vendor", 95);
   add_list_column(G_gui.status_list, 7, "Product", 95);
   add_list_column(G_gui.status_list, 8, "Rev", 95);
   add_list_column(G_gui.status_list, 9, "DC", 45);
   add_list_column(G_gui.status_list, 10, "DB Match", 145);

   ListView_SetExtendedListViewStyle(G_gui.xml_list,
                                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
   add_list_column(G_gui.xml_list, 0, "#", 42);
   add_list_column(G_gui.xml_list, 1, "Name", 175);
   add_list_column(G_gui.xml_list, 2, "Vendor", 90);
   add_list_column(G_gui.xml_list, 3, "Product", 90);
   add_list_column(G_gui.xml_list, 4, "Rev", 90);
   add_list_column(G_gui.xml_list, 5, "Type", 55);
   add_list_column(G_gui.xml_list, 6, "XML", 280);

   ListView_SetExtendedListViewStyle(G_gui.coe_list,
                                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
   add_list_column(G_gui.coe_list, 0, "Index", 72);
   add_list_column(G_gui.coe_list, 1, "Sub", 46);
   add_list_column(G_gui.coe_list, 2, "Name", 210);
   add_list_column(G_gui.coe_list, 3, "Flags", 70);
   add_list_column(G_gui.coe_list, 4, "Value", 320);
   add_list_column(G_gui.coe_list, 5, "Online", 70);
}

static void add_tab(HWND tabs, const char *title, int index)
{
   TCITEMA item;
   memset(&item, 0, sizeof(item));
   item.mask = TCIF_TEXT;
   item.pszText = (LPSTR)title;
   TabCtrl_InsertItem(tabs, index, &item);
}

static void show_tab_controls(int tab)
{
   int show_sdo = (tab == 2) ? SW_SHOW : SW_HIDE;
   int show_coe = (tab == 3) ? SW_SHOW : SW_HIDE;
   int show_motion = (tab == 4) ? SW_SHOW : SW_HIDE;
   int i;

   ShowWindow(G_gui.status_list, tab == 0 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.pdo_edit, tab == 1 ? SW_SHOW : SW_HIDE);

   for (i = 0; i < 4; ++i)
   {
      ShowWindow(G_gui.sdo_labels[i], show_sdo);
   }
   ShowWindow(G_gui.sdo_slave, show_sdo);
   ShowWindow(G_gui.sdo_index, show_sdo);
   ShowWindow(G_gui.sdo_sub, show_sdo);
   ShowWindow(G_gui.sdo_size, show_sdo);
   ShowWindow(G_gui.sdo_read, show_sdo);
   ShowWindow(G_gui.sdo_result, show_sdo);

   ShowWindow(G_gui.coe_update, show_coe);
   ShowWindow(G_gui.coe_auto_update, show_coe);
   ShowWindow(G_gui.coe_single_update, show_coe);
   ShowWindow(G_gui.coe_show_offline, show_coe);
   ShowWindow(G_gui.coe_slave, show_coe);
   ShowWindow(G_gui.coe_list, show_coe);
   ShowWindow(G_gui.coe_detail, show_coe);
   for (i = 0; i < 1; ++i)
   {
      if (G_gui.coe_labels[i] != NULL)
      {
         ShowWindow(G_gui.coe_labels[i], show_coe);
      }
   }

   for (i = 0; i < 15; ++i)
   {
      if (G_gui.motion_labels[i] != NULL)
      {
         ShowWindow(G_gui.motion_labels[i], show_motion);
      }
   }
   ShowWindow(G_gui.motion_profile, show_motion);
   ShowWindow(G_gui.motion_slave, show_motion);
   ShowWindow(G_gui.motion_target, show_motion);
   ShowWindow(G_gui.motion_position2, show_motion);
   ShowWindow(G_gui.motion_velocity, show_motion);
   ShowWindow(G_gui.motion_accel, show_motion);
   ShowWindow(G_gui.motion_decel, show_motion);
   ShowWindow(G_gui.motion_home_method, show_motion);
   ShowWindow(G_gui.motion_command_pos, show_motion);
   ShowWindow(G_gui.motion_actual_pos, show_motion);
   ShowWindow(G_gui.motion_op_status, show_motion);
   ShowWindow(G_gui.motion_command_vel, show_motion);
   ShowWindow(G_gui.motion_actual_vel, show_motion);
   ShowWindow(G_gui.motion_fault_reset, show_motion);
   ShowWindow(G_gui.motion_enable, show_motion);
   ShowWindow(G_gui.motion_disable, show_motion);
   ShowWindow(G_gui.motion_stop, show_motion);
   ShowWindow(G_gui.motion_jog_pos, show_motion);
   ShowWindow(G_gui.motion_jog_neg, show_motion);
   ShowWindow(G_gui.motion_move_abs, show_motion);
   ShowWindow(G_gui.motion_move_abs2, show_motion);
   ShowWindow(G_gui.motion_move_rel, show_motion);
   ShowWindow(G_gui.motion_home, show_motion);
   ShowWindow(G_gui.motion_repeat, show_motion);
   ShowWindow(G_gui.motion_delay, show_motion);
   ShowWindow(G_gui.motion_check_inpos, show_motion);
   ShowWindow(G_gui.motion_status, show_motion);

   ShowWindow(G_gui.stats_edit, tab == 5 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_import, tab == 6 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_reload, tab == 6 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_list, tab == 6 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_detail, tab == 6 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.log_edit, tab == 7 ? SW_SHOW : SW_HIDE);
}

static void bind_controls(HWND hwnd)
{
   G_gui.hwnd = hwnd;
   G_gui.backend = GetDlgItem(hwnd, IDC_BACKEND);
   G_gui.rt_host = GetDlgItem(hwnd, IDC_RT_HOST);
   G_gui.rt_port = GetDlgItem(hwnd, IDC_RT_PORT);
   G_gui.adapter = GetDlgItem(hwnd, IDC_ADAPTER);
   G_gui.refresh = GetDlgItem(hwnd, IDC_REFRESH);
   G_gui.connect = GetDlgItem(hwnd, IDC_CONNECT);
   G_gui.disconnect = GetDlgItem(hwnd, IDC_DISCONNECT);
   G_gui.opmode = GetDlgItem(hwnd, IDC_OPMODE);
   G_gui.period = GetDlgItem(hwnd, IDC_PERIOD);
   G_gui.summary = GetDlgItem(hwnd, IDC_SUMMARY);
   G_gui.slaves = GetDlgItem(hwnd, IDC_SLAVES);
   G_gui.tabs = GetDlgItem(hwnd, IDC_TABS);
   G_gui.status_list = GetDlgItem(hwnd, IDC_STATUS_LIST);
   G_gui.pdo_edit = GetDlgItem(hwnd, IDC_PDO_EDIT);
   G_gui.sdo_slave = GetDlgItem(hwnd, IDC_SDO_SLAVE);
   G_gui.sdo_index = GetDlgItem(hwnd, IDC_SDO_INDEX);
   G_gui.sdo_sub = GetDlgItem(hwnd, IDC_SDO_SUB);
   G_gui.sdo_size = GetDlgItem(hwnd, IDC_SDO_SIZE);
   G_gui.sdo_read = GetDlgItem(hwnd, IDC_SDO_READ);
   G_gui.sdo_result = GetDlgItem(hwnd, IDC_SDO_RESULT);
   G_gui.motion_slave = GetDlgItem(hwnd, IDC_MOTION_SLAVE);
   G_gui.motion_target = GetDlgItem(hwnd, IDC_MOTION_TARGET);
   G_gui.motion_position2 = GetDlgItem(hwnd, IDC_MOTION_POSITION2);
   G_gui.motion_velocity = GetDlgItem(hwnd, IDC_MOTION_VELOCITY);
   G_gui.motion_accel = GetDlgItem(hwnd, IDC_MOTION_ACCEL);
   G_gui.motion_decel = GetDlgItem(hwnd, IDC_MOTION_DECEL);
   G_gui.motion_home_method = GetDlgItem(hwnd, IDC_MOTION_HOME_METHOD);
   G_gui.motion_profile = GetDlgItem(hwnd, IDC_MOTION_PROFILE);
   G_gui.motion_command_pos = GetDlgItem(hwnd, IDC_MOTION_COMMAND_POS);
   G_gui.motion_actual_pos = GetDlgItem(hwnd, IDC_MOTION_ACTUAL_POS);
   G_gui.motion_op_status = GetDlgItem(hwnd, IDC_MOTION_OP_STATUS);
   G_gui.motion_command_vel = GetDlgItem(hwnd, IDC_MOTION_COMMAND_VEL);
   G_gui.motion_actual_vel = GetDlgItem(hwnd, IDC_MOTION_ACTUAL_VEL);
   G_gui.motion_fault_reset = GetDlgItem(hwnd, IDC_MOTION_FAULT_RESET);
   G_gui.motion_enable = GetDlgItem(hwnd, IDC_MOTION_ENABLE);
   G_gui.motion_disable = GetDlgItem(hwnd, IDC_MOTION_DISABLE);
   G_gui.motion_stop = GetDlgItem(hwnd, IDC_MOTION_STOP);
   G_gui.motion_jog_pos = GetDlgItem(hwnd, IDC_MOTION_JOG_POS);
   G_gui.motion_jog_neg = GetDlgItem(hwnd, IDC_MOTION_JOG_NEG);
   G_gui.motion_move_abs = GetDlgItem(hwnd, IDC_MOTION_MOVE_ABS);
   G_gui.motion_move_abs2 = GetDlgItem(hwnd, IDC_MOTION_MOVE_ABS2);
   G_gui.motion_move_rel = GetDlgItem(hwnd, IDC_MOTION_MOVE_REL);
   G_gui.motion_home = GetDlgItem(hwnd, IDC_MOTION_HOME);
   G_gui.motion_repeat = GetDlgItem(hwnd, IDC_MOTION_REPEAT);
   G_gui.motion_delay = GetDlgItem(hwnd, IDC_MOTION_DELAY);
   G_gui.motion_check_inpos = GetDlgItem(hwnd, IDC_MOTION_CHECK_INPOS);
   G_gui.motion_status = GetDlgItem(hwnd, IDC_MOTION_STATUS);
   G_gui.coe_update = GetDlgItem(hwnd, IDC_COE_UPDATE);
   G_gui.coe_auto_update = GetDlgItem(hwnd, IDC_COE_AUTO_UPDATE);
   G_gui.coe_single_update = GetDlgItem(hwnd, IDC_COE_SINGLE_UPDATE);
   G_gui.coe_show_offline = GetDlgItem(hwnd, IDC_COE_SHOW_OFFLINE);
   G_gui.coe_slave = GetDlgItem(hwnd, IDC_COE_SLAVE);
   G_gui.coe_list = GetDlgItem(hwnd, IDC_COE_LIST);
   G_gui.coe_detail = GetDlgItem(hwnd, IDC_COE_DETAIL);
   G_gui.stats_edit = GetDlgItem(hwnd, IDC_STATS_EDIT);
   G_gui.xml_import = GetDlgItem(hwnd, IDC_XML_IMPORT);
   G_gui.xml_reload = GetDlgItem(hwnd, IDC_XML_RELOAD);
   G_gui.xml_list = GetDlgItem(hwnd, IDC_XML_LIST);
   G_gui.xml_detail = GetDlgItem(hwnd, IDC_XML_DETAIL);
   G_gui.log_edit = GetDlgItem(hwnd, IDC_LOG_EDIT);
   G_gui.sdo_labels[0] = GetDlgItem(hwnd, IDC_SDO_SLAVE_LABEL);
   G_gui.sdo_labels[1] = GetDlgItem(hwnd, IDC_SDO_INDEX_LABEL);
   G_gui.sdo_labels[2] = GetDlgItem(hwnd, IDC_SDO_SUB_LABEL);
   G_gui.sdo_labels[3] = GetDlgItem(hwnd, IDC_SDO_SIZE_LABEL);
   G_gui.motion_labels[0] = GetDlgItem(hwnd, IDC_MOTION_SLAVE_LABEL);
   G_gui.motion_labels[1] = GetDlgItem(hwnd, IDC_MOTION_TARGET_LABEL);
   G_gui.motion_labels[2] = GetDlgItem(hwnd, IDC_MOTION_VELOCITY_LABEL);
   G_gui.motion_labels[3] = GetDlgItem(hwnd, IDC_MOTION_ACCEL_LABEL);
   G_gui.motion_labels[4] = GetDlgItem(hwnd, IDC_MOTION_DECEL_LABEL);
   G_gui.motion_labels[5] = GetDlgItem(hwnd, IDC_MOTION_HOME_METHOD_LABEL);
   G_gui.motion_labels[6] = GetDlgItem(hwnd, IDC_MOTION_PROFILE_LABEL);
   G_gui.motion_labels[7] = GetDlgItem(hwnd, IDC_MOTION_COMMAND_POS_LABEL);
   G_gui.motion_labels[8] = GetDlgItem(hwnd, IDC_MOTION_ACTUAL_POS_LABEL);
   G_gui.motion_labels[9] = GetDlgItem(hwnd, IDC_MOTION_OP_STATUS_LABEL);
   G_gui.motion_labels[10] = GetDlgItem(hwnd, IDC_MOTION_COMMAND_VEL_LABEL);
   G_gui.motion_labels[11] = GetDlgItem(hwnd, IDC_MOTION_ACTUAL_VEL_LABEL);
   G_gui.motion_labels[12] = GetDlgItem(hwnd, IDC_MOTION_POSITION2_LABEL);
   G_gui.motion_labels[13] = GetDlgItem(hwnd, IDC_MOTION_DELAY_LABEL);
   G_gui.motion_labels[14] = NULL;
   G_gui.coe_labels[0] = GetDlgItem(hwnd, IDC_COE_SLAVE_LABEL);
   G_gui.rt_labels[0] = GetDlgItem(hwnd, IDC_RT_HOST_LABEL);
   G_gui.rt_labels[1] = GetDlgItem(hwnd, IDC_RT_PORT_LABEL);

   SendMessageA(G_gui.backend, CB_ADDSTRING, 0, (LPARAM)"Windows Local");
   SendMessageA(G_gui.backend, CB_ADDSTRING, 0, (LPARAM)"Linux RT");
   SendMessageA(G_gui.backend, CB_SETCURSEL, 0, 0);
   SendMessageA(G_gui.motion_profile, CB_ADDSTRING, 0, (LPARAM)"0 LMS");
   SendMessageA(G_gui.motion_profile, CB_ADDSTRING, 0, (LPARAM)"1 Trapezoidal");
   SendMessageA(G_gui.motion_profile, CB_ADDSTRING, 0, (LPARAM)"2 SCurve");
   SendMessageA(G_gui.motion_profile, CB_ADDSTRING, 0, (LPARAM)"3 JerkRatio");
   SendMessageA(G_gui.motion_profile, CB_SETCURSEL, 1, 0);
   SendMessageA(G_gui.rt_host, EM_SETLIMITTEXT, 63, 0);
   SendMessageA(G_gui.rt_port, EM_SETLIMITTEXT, 5, 0);
   SendMessageA(G_gui.period, EM_SETLIMITTEXT, 8, 0);
   SendMessageA(G_gui.sdo_index, EM_SETLIMITTEXT, 12, 0);
   SendMessageA(G_gui.sdo_sub, EM_SETLIMITTEXT, 8, 0);
   SendMessageA(G_gui.sdo_size, EM_SETLIMITTEXT, 4, 0);
   SendMessageA(G_gui.motion_slave, EM_SETLIMITTEXT, 4, 0);
   SendMessageA(G_gui.motion_target, EM_SETLIMITTEXT, 12, 0);
   SendMessageA(G_gui.motion_position2, EM_SETLIMITTEXT, 12, 0);
   SendMessageA(G_gui.motion_velocity, EM_SETLIMITTEXT, 12, 0);
   SendMessageA(G_gui.motion_accel, EM_SETLIMITTEXT, 10, 0);
   SendMessageA(G_gui.motion_decel, EM_SETLIMITTEXT, 10, 0);
   SendMessageA(G_gui.motion_home_method, EM_SETLIMITTEXT, 6, 0);
   SendMessageA(G_gui.motion_delay, EM_SETLIMITTEXT, 8, 0);
   SendMessageA(G_gui.coe_slave, EM_SETLIMITTEXT, 4, 0);
   SetWindowTextA(G_gui.period, "1000");
   SetWindowTextA(G_gui.rt_host, "192.168.100.20");
   SetWindowTextA(G_gui.rt_port, "15000");
   SetWindowTextA(G_gui.sdo_slave, "1");
   SetWindowTextA(G_gui.sdo_index, "0x6041");
   SetWindowTextA(G_gui.sdo_sub, "0");
   SetWindowTextA(G_gui.sdo_size, "64");
   SetWindowTextA(G_gui.motion_slave, "1");
   SetWindowTextA(G_gui.motion_target, "320000");
   SetWindowTextA(G_gui.motion_position2, "180000");
   SetWindowTextA(G_gui.motion_velocity, "1000000");
   SetWindowTextA(G_gui.motion_accel, "2000000");
   SetWindowTextA(G_gui.motion_decel, "2000000");
   SetWindowTextA(G_gui.motion_home_method, "35");
   SetWindowTextA(G_gui.motion_delay, "3000");
   SetWindowTextA(G_gui.coe_slave, "1");
   SendMessageA(G_gui.coe_single_update, BM_SETCHECK, BST_CHECKED, 0);
   SendMessageA(G_gui.coe_show_offline, BM_SETCHECK, BST_CHECKED, 0);

   add_tab(G_gui.tabs, "Slave Status", 0);
   add_tab(G_gui.tabs, "PDO Monitor", 1);
   add_tab(G_gui.tabs, "SDO Browser", 2);
   add_tab(G_gui.tabs, "CoE Online", 3);
   add_tab(G_gui.tabs, "Motion Control", 4);
   add_tab(G_gui.tabs, "Communication", 5);
   add_tab(G_gui.tabs, "XML Database", 6);
   add_tab(G_gui.tabs, "Log", 7);
   init_list_columns();
   G_gui.active_tab = 0;
   G_gui.selected_slave = 1;
   show_tab_controls(0);
}

static void layout_controls(HWND hwnd)
{
   RECT rc;
   int w;
   int h;
   int margin = 8;
   int top_h = 116;
   int left_w = 330;
   int body_y;
   int body_h;
   int right_x;
   int right_w;
   int tab_y;
   RECT tab_rc;

   GetClientRect(hwnd, &rc);
   w = rc.right - rc.left;
   h = rc.bottom - rc.top;

   MoveWindow(G_gui.backend, margin, margin, 112, 120, TRUE);
   MoveWindow(G_gui.rt_labels[0], margin + 120, margin + 5, 32, 18, TRUE);
   MoveWindow(G_gui.rt_host, margin + 152, margin, 150, 25, TRUE);
   MoveWindow(G_gui.rt_labels[1], margin + 310, margin + 5, 30, 18, TRUE);
   MoveWindow(G_gui.rt_port, margin + 340, margin, 60, 25, TRUE);
   MoveWindow(G_gui.refresh, margin + 408, margin, 70, 25, TRUE);
   MoveWindow(G_gui.connect, margin + 484, margin, 74, 25, TRUE);
   MoveWindow(G_gui.disconnect, margin + 564, margin, 88, 25, TRUE);
   MoveWindow(G_gui.adapter, margin, margin + 32, 500, 220, TRUE);
   MoveWindow(G_gui.opmode, margin + 508, margin + 35, 92, 22, TRUE);
   MoveWindow(GetDlgItem(hwnd, IDC_PERIOD_LABEL), margin + 608, margin + 37,
              66, 18, TRUE);
   MoveWindow(G_gui.period, margin + 676, margin + 32, 68, 25, TRUE);
   MoveWindow(G_gui.summary, margin, margin + 64, w - margin * 2, 42, TRUE);

   body_y = top_h;
   body_h = h - top_h - margin;
   right_x = margin + left_w + margin;
   right_w = w - right_x - margin;

   MoveWindow(G_gui.slaves, margin, body_y, left_w, body_h, TRUE);
   MoveWindow(G_gui.tabs, right_x, body_y, right_w, body_h, TRUE);

   tab_rc.left = right_x + 8;
   tab_rc.top = body_y + 30;
   tab_rc.right = right_x + right_w - 8;
   tab_rc.bottom = body_y + body_h - 8;
   tab_y = tab_rc.top;

   MoveWindow(G_gui.status_list, tab_rc.left, tab_rc.top,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_rc.top, TRUE);
   MoveWindow(G_gui.pdo_edit, tab_rc.left, tab_rc.top,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_rc.top, TRUE);

   MoveWindow(G_gui.sdo_labels[0], tab_rc.left, tab_y + 4, 40, 22, TRUE);
   MoveWindow(G_gui.sdo_slave, tab_rc.left + 44, tab_y, 58, 24, TRUE);
   MoveWindow(G_gui.sdo_labels[1], tab_rc.left + 112, tab_y + 4, 42, 22, TRUE);
   MoveWindow(G_gui.sdo_index, tab_rc.left + 154, tab_y, 82, 24, TRUE);
   MoveWindow(G_gui.sdo_labels[2], tab_rc.left + 246, tab_y + 4, 34, 22, TRUE);
   MoveWindow(G_gui.sdo_sub, tab_rc.left + 280, tab_y, 56, 24, TRUE);
   MoveWindow(G_gui.sdo_labels[3], tab_rc.left + 346, tab_y + 4, 34, 22, TRUE);
   MoveWindow(G_gui.sdo_size, tab_rc.left + 382, tab_y, 58, 24, TRUE);
   MoveWindow(G_gui.sdo_read, tab_rc.left + 452, tab_y, 90, 24, TRUE);
   MoveWindow(G_gui.sdo_result, tab_rc.left, tab_y + 34,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_y - 34, TRUE);

   MoveWindow(G_gui.motion_labels[7], tab_rc.left, tab_y + 4, 78, 22, TRUE);
   MoveWindow(G_gui.motion_command_pos, tab_rc.left + 82, tab_y, 92, 24, TRUE);
   MoveWindow(G_gui.motion_labels[8], tab_rc.left + 184, tab_y + 4, 70, 22, TRUE);
   MoveWindow(G_gui.motion_actual_pos, tab_rc.left + 258, tab_y, 92, 24, TRUE);
   MoveWindow(G_gui.motion_labels[9], tab_rc.left + 360, tab_y + 4, 68, 22, TRUE);
   MoveWindow(G_gui.motion_op_status, tab_rc.left + 432, tab_y, 128, 24, TRUE);
   MoveWindow(G_gui.motion_labels[10], tab_rc.left, tab_y + 30, 78, 22, TRUE);
   MoveWindow(G_gui.motion_command_vel, tab_rc.left + 82, tab_y + 26, 92, 24, TRUE);
   MoveWindow(G_gui.motion_labels[11], tab_rc.left + 184, tab_y + 30, 70, 22, TRUE);
   MoveWindow(G_gui.motion_actual_vel, tab_rc.left + 258, tab_y + 26, 92, 24, TRUE);
   MoveWindow(G_gui.motion_labels[0], tab_rc.left + 360, tab_y + 30, 40, 22, TRUE);
   MoveWindow(G_gui.motion_slave, tab_rc.left + 402, tab_y + 26, 50, 24, TRUE);
   MoveWindow(G_gui.motion_labels[5], tab_rc.left + 464, tab_y + 30, 44, 22, TRUE);
   MoveWindow(G_gui.motion_home_method, tab_rc.left + 510, tab_y + 26, 50, 24, TRUE);

   MoveWindow(G_gui.motion_fault_reset, tab_rc.left, tab_y + 60, 96, 26, TRUE);
   MoveWindow(G_gui.motion_enable, tab_rc.left + 104, tab_y + 60, 76, 26, TRUE);
   MoveWindow(G_gui.motion_disable, tab_rc.left + 188, tab_y + 60, 76, 26, TRUE);
   MoveWindow(G_gui.motion_stop, tab_rc.left + 272, tab_y + 60, 76, 26, TRUE);
   MoveWindow(G_gui.motion_jog_pos, tab_rc.left + 360, tab_y + 60, 72, 26, TRUE);
   MoveWindow(G_gui.motion_jog_neg, tab_rc.left + 440, tab_y + 60, 72, 26, TRUE);
   MoveWindow(G_gui.motion_home, tab_rc.left + 520, tab_y + 60, 60, 26, TRUE);

   MoveWindow(G_gui.motion_labels[6], tab_rc.left, tab_y + 98, 82, 22, TRUE);
   MoveWindow(G_gui.motion_profile, tab_rc.left + 86, tab_y + 94, 148, 120, TRUE);
   MoveWindow(G_gui.motion_labels[2], tab_rc.left + 250, tab_y + 98, 58, 22, TRUE);
   MoveWindow(G_gui.motion_velocity, tab_rc.left + 312, tab_y + 94, 96, 24, TRUE);
   MoveWindow(G_gui.motion_labels[3], tab_rc.left + 422, tab_y + 98, 44, 22, TRUE);
   MoveWindow(G_gui.motion_accel, tab_rc.left + 468, tab_y + 94, 82, 24, TRUE);
   MoveWindow(G_gui.motion_labels[4], tab_rc.left + 562, tab_y + 98, 44, 22, TRUE);
   MoveWindow(G_gui.motion_decel, tab_rc.left + 608, tab_y + 94, 82, 24, TRUE);

   MoveWindow(G_gui.motion_labels[1], tab_rc.left, tab_y + 132, 64, 22, TRUE);
   MoveWindow(G_gui.motion_target, tab_rc.left + 68, tab_y + 128, 96, 24, TRUE);
   MoveWindow(G_gui.motion_move_abs, tab_rc.left + 172, tab_y + 128, 82, 26, TRUE);
   MoveWindow(G_gui.motion_labels[12], tab_rc.left + 270, tab_y + 132, 64, 22, TRUE);
   MoveWindow(G_gui.motion_position2, tab_rc.left + 338, tab_y + 128, 96, 24, TRUE);
   MoveWindow(G_gui.motion_move_abs2, tab_rc.left + 442, tab_y + 128, 82, 26, TRUE);
   MoveWindow(G_gui.motion_move_rel, tab_rc.left + 532, tab_y + 128, 82, 26, TRUE);

   MoveWindow(G_gui.motion_repeat, tab_rc.left, tab_y + 162, 82, 28, TRUE);
   MoveWindow(G_gui.motion_labels[13], tab_rc.left + 96, tab_y + 168, 62, 22, TRUE);
   MoveWindow(G_gui.motion_delay, tab_rc.left + 162, tab_y + 164, 80, 24, TRUE);
   MoveWindow(G_gui.motion_check_inpos, tab_rc.left + 256, tab_y + 167, 96, 22, TRUE);
   MoveWindow(G_gui.motion_status, tab_rc.left, tab_y + 202,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_y - 202, TRUE);

   MoveWindow(G_gui.coe_update, tab_rc.left, tab_y, 92, 24, TRUE);
   MoveWindow(G_gui.coe_auto_update, tab_rc.left + 104, tab_y + 4, 96, 22, TRUE);
   MoveWindow(G_gui.coe_single_update, tab_rc.left + 210, tab_y + 4, 104, 22, TRUE);
   MoveWindow(G_gui.coe_show_offline, tab_rc.left + 324, tab_y + 4, 132, 22, TRUE);
   MoveWindow(G_gui.coe_labels[0], tab_rc.left + 470, tab_y + 4, 38, 22, TRUE);
   MoveWindow(G_gui.coe_slave, tab_rc.left + 510, tab_y, 50, 24, TRUE);
   MoveWindow(G_gui.coe_list, tab_rc.left, tab_y + 34,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_y - 132, TRUE);
   MoveWindow(G_gui.coe_detail, tab_rc.left, tab_rc.bottom - 90,
              tab_rc.right - tab_rc.left, 90, TRUE);

   MoveWindow(G_gui.stats_edit, tab_rc.left, tab_rc.top,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_rc.top, TRUE);
   MoveWindow(G_gui.xml_import, tab_rc.left, tab_y, 82, 24, TRUE);
   MoveWindow(G_gui.xml_reload, tab_rc.left + 90, tab_y, 82, 24, TRUE);
   MoveWindow(G_gui.xml_list, tab_rc.left, tab_y + 34,
              tab_rc.right - tab_rc.left,
              (tab_rc.bottom - tab_y - 34) * 2 / 3, TRUE);
   MoveWindow(G_gui.xml_detail, tab_rc.left,
              tab_y + 42 + (tab_rc.bottom - tab_y - 34) * 2 / 3,
              tab_rc.right - tab_rc.left,
              tab_rc.bottom - (tab_y + 42 +
                               (tab_rc.bottom - tab_y - 34) * 2 / 3),
              TRUE);
   MoveWindow(G_gui.log_edit, tab_rc.left, tab_rc.top,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_rc.top, TRUE);
}

static int apply_backend_settings(void)
{
   int backend_index;
   int backend;
   char host[64];
   char port_text[16];
   int port;
   int result;

   backend_index = (int)SendMessageA(G_gui.backend, CB_GETCURSEL, 0, 0);
   backend = backend_index == 1 ? ECAT_BACKEND_LINUX_RT
                                : ECAT_BACKEND_WINDOWS_DEBUG;

   result = ECAT_SetBackend(backend);
   if (result == ECAT_BUSY)
   {
      return ECAT_OK;
   }
   if (result != ECAT_OK)
   {
      return result;
   }

   if (backend == ECAT_BACKEND_LINUX_RT)
   {
      GetWindowTextA(G_gui.rt_host, host, sizeof(host));
      GetWindowTextA(G_gui.rt_port, port_text, sizeof(port_text));
      port = atoi(port_text);
      if (port <= 0)
      {
         port = 15000;
         SetWindowTextA(G_gui.rt_port, "15000");
      }
      if (host[0] == '\0')
      {
         safe_copy(host, sizeof(host), "127.0.0.1");
         SetWindowTextA(G_gui.rt_host, host);
      }
      result = ECAT_SetLinuxRtEndpoint(host, port);
   }

   return result;
}

static void refresh_adapters(void)
{
   ECAT_AdapterInfo adapters[64];
   int count = 0;
   int i;

   (void)apply_backend_settings();
   SendMessageA(G_gui.adapter, CB_RESETCONTENT, 0, 0);
   if (ECAT_ListAdapters(adapters, 64, &count) != ECAT_OK)
   {
      append_log_line("ERROR", "ECAT_ListAdapters failed");
      return;
   }

   for (i = 0; i < count && i < 64; ++i)
   {
      SendMessageA(G_gui.adapter, CB_ADDSTRING, 0, (LPARAM)adapters[i].name);
   }
   if (count > 0)
   {
      SendMessageA(G_gui.adapter, CB_SETCURSEL, 0, 0);
      append_log_line("INFO", "Adapter list refreshed");
   }
   else
   {
      append_log_line("WARN", "No pcap adapters found");
   }
}

static void insert_row(HWND list, int row, const char *text)
{
   LVITEMA item;
   memset(&item, 0, sizeof(item));
   item.mask = LVIF_TEXT;
   item.iItem = row;
   item.iSubItem = 0;
   item.pszText = (LPSTR)text;
   ListView_InsertItem(list, &item);
}

static void set_row_text(HWND list, int row, int col, const char *text)
{
   ListView_SetItemText(list, row, col, (LPSTR)text);
}

static void update_list_views(const GuiSnapshot *snapshot)
{
   int i;
   int count;

   SendMessageA(G_gui.slaves, WM_SETREDRAW, FALSE, 0);
   SendMessageA(G_gui.status_list, WM_SETREDRAW, FALSE, 0);
   ListView_DeleteAllItems(G_gui.slaves);
   ListView_DeleteAllItems(G_gui.status_list);

   count = snapshot->runtime.slave_count;
   if (count > GUI_MAX_SLAVES)
   {
      count = GUI_MAX_SLAVES;
   }

   for (i = 1; i <= count; ++i)
   {
      const GuiSlaveSnapshot *slave = &snapshot->slaves[i - 1];
      const ECAT_SlaveInfo *s = &slave->info;
      char b0[64], b1[64], b2[64], b3[64], b4[64];

      if (!slave->valid)
      {
         continue;
      }

      (void)snprintf(b0, sizeof(b0), "%d", s->index);
      insert_row(G_gui.slaves, i - 1, b0);
      set_row_text(G_gui.slaves, i - 1, 1, s->name);
      set_row_text(G_gui.slaves, i - 1, 2, ECAT_StateName(s->state));
      (void)snprintf(b1, sizeof(b1), "%uO/%uI", s->output_bytes,
                     s->input_bytes);
      set_row_text(G_gui.slaves, i - 1, 3, b1);

      insert_row(G_gui.status_list, i - 1, b0);
      set_row_text(G_gui.status_list, i - 1, 1, s->name);
      set_row_text(G_gui.status_list, i - 1, 2, ECAT_StateName(s->state));
      (void)snprintf(b1, sizeof(b1), "0x%04X", s->al_status);
      set_row_text(G_gui.status_list, i - 1, 3, b1);
      (void)snprintf(b2, sizeof(b2), "%u B", s->output_bytes);
      (void)snprintf(b3, sizeof(b3), "%u B", s->input_bytes);
      set_row_text(G_gui.status_list, i - 1, 4, b2);
      set_row_text(G_gui.status_list, i - 1, 5, b3);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s->vendor_id);
      set_row_text(G_gui.status_list, i - 1, 6, b4);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s->product_code);
      set_row_text(G_gui.status_list, i - 1, 7, b4);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s->revision);
      set_row_text(G_gui.status_list, i - 1, 8, b4);
      set_row_text(G_gui.status_list, i - 1, 9, s->has_dc ? "Yes" : "No");
      set_row_text(G_gui.status_list, i - 1, 10,
                   s->database_matched ? s->database_name : "-");

      if (s->index == G_gui.selected_slave)
      {
         ListView_SetItemState(G_gui.slaves, i - 1,
                               LVIS_SELECTED | LVIS_FOCUSED,
                               LVIS_SELECTED | LVIS_FOCUSED);
      }
   }
   SendMessageA(G_gui.slaves, WM_SETREDRAW, TRUE, 0);
   SendMessageA(G_gui.status_list, WM_SETREDRAW, TRUE, 0);
   InvalidateRect(G_gui.slaves, NULL, TRUE);
   InvalidateRect(G_gui.status_list, NULL, TRUE);
}

static void format_xml_detail(char *dst, size_t dst_size,
                              const ECAT_DbEntry *entry)
{
   char root[ECAT_MAX_PATH_TEXT];

   if (entry == NULL)
   {
      int count = 0;
      root[0] = '\0';
      ECAT_DbGetRoot(root, sizeof(root));
      ECAT_DbGetCount(&count);
      (void)snprintf(dst, dst_size,
                     "Database root\r\n  %s\r\n\r\n"
                     "Registered slaves\r\n  %d\r\n",
                     root, count);
      return;
   }

   (void)snprintf(dst, dst_size,
                  "Name\r\n  %s\r\n\r\n"
                  "Identity\r\n"
                  "  Vendor ID     : 0x%08X\r\n"
                  "  Product Code  : 0x%08X\r\n"
                  "  Revision      : 0x%08X\r\n"
                  "  XML Type      : %s\r\n"
                  "  Imported      : %s\r\n\r\n"
                  "Stored XML\r\n  %s\r\n",
                  entry->name, entry->vendor_id, entry->product_code,
                  entry->revision, entry->xml_type, entry->imported_at,
                  entry->xml_path);
}

static void update_xml_detail(int index)
{
   ECAT_DbEntry entry;
   char text[4096];

   if (index >= 0 && ECAT_DbGetEntry(index, &entry) == ECAT_OK)
   {
      format_xml_detail(text, sizeof(text), &entry);
   }
   else
   {
      format_xml_detail(text, sizeof(text), NULL);
   }
   SetWindowTextA(G_gui.xml_detail, text);
}

static void update_xml_list(void)
{
   int count = 0;
   int i;

   ListView_DeleteAllItems(G_gui.xml_list);
   if (ECAT_DbGetCount(&count) != ECAT_OK)
   {
      append_log_line("ERROR", "ECAT_DbGetCount failed");
      update_xml_detail(-1);
      return;
   }

   for (i = 0; i < count; ++i)
   {
      ECAT_DbEntry entry;
      char b0[32], b1[64];

      if (ECAT_DbGetEntry(i, &entry) != ECAT_OK)
      {
         continue;
      }
      (void)snprintf(b0, sizeof(b0), "%d", i + 1);
      insert_row(G_gui.xml_list, i, b0);
      set_row_text(G_gui.xml_list, i, 1, entry.name);
      (void)snprintf(b1, sizeof(b1), "0x%08X", entry.vendor_id);
      set_row_text(G_gui.xml_list, i, 2, b1);
      (void)snprintf(b1, sizeof(b1), "0x%08X", entry.product_code);
      set_row_text(G_gui.xml_list, i, 3, b1);
      (void)snprintf(b1, sizeof(b1), "0x%08X", entry.revision);
      set_row_text(G_gui.xml_list, i, 4, b1);
      set_row_text(G_gui.xml_list, i, 5, entry.xml_type);
      set_row_text(G_gui.xml_list, i, 6, entry.xml_path);
   }
   update_xml_detail(-1);
}

static void reload_xml_db(void)
{
   int result = ECAT_DbReload();
   if (result != ECAT_OK)
   {
      append_log_line("ERROR", "ECAT_DbReload failed");
      MessageBoxA(G_gui.hwnd, ECAT_ErrorToString(result), APP_TITLE,
                  MB_ICONERROR | MB_OK);
      return;
   }
   update_xml_list();
   append_log_line("INFO", "XML database reloaded");
}

static void import_xml_file(void)
{
   OPENFILENAMEA ofn;
   char path[MAX_PATH];
   ECAT_DbEntry imported;
   int result;

   memset(path, 0, sizeof(path));
   memset(&ofn, 0, sizeof(ofn));
   ofn.lStructSize = sizeof(ofn);
   ofn.hwndOwner = G_gui.hwnd;
   ofn.lpstrFilter = "EtherCAT XML (*.xml)\0*.xml\0All files (*.*)\0*.*\0";
   ofn.lpstrFile = path;
   ofn.nMaxFile = sizeof(path);
   ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
   ofn.lpstrTitle = "Import EtherCAT ESI/ENI XML";

   if (!GetOpenFileNameA(&ofn))
   {
      return;
   }

   memset(&imported, 0, sizeof(imported));
   result = ECAT_DbImportXml(path, &imported);
   if (result != ECAT_OK)
   {
      char message[512];
      (void)snprintf(message, sizeof(message),
                     "XML import failed.\r\n%s\r\n\r\n"
                     "Check that the XML contains Vendor Id and ProductCode.",
                     ECAT_ErrorToString(result));
      MessageBoxA(G_gui.hwnd, message, APP_TITLE, MB_ICONERROR | MB_OK);
      append_log_line("ERROR", message);
      return;
   }

   update_xml_list();
   append_log_line("INFO", "XML file imported into database");
   ListView_SetItemState(G_gui.xml_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                         LVIS_SELECTED | LVIS_FOCUSED);
   update_xml_detail(0);
}

static const GuiSlaveSnapshot *snapshot_find_slave(const GuiSnapshot *snapshot,
                                                   int slave_index)
{
   int i;
   int count;

   if (snapshot == NULL || slave_index <= 0)
   {
      return NULL;
   }
   count = snapshot->runtime.slave_count;
   if (count > GUI_MAX_SLAVES)
   {
      count = GUI_MAX_SLAVES;
   }
   for (i = 0; i < count; ++i)
   {
      if (snapshot->slaves[i].valid &&
          snapshot->slaves[i].info.index == slave_index)
      {
         return &snapshot->slaves[i];
      }
   }
   return NULL;
}

static void format_pdo_text(char *dst, size_t dst_size,
                            const GuiSnapshot *snapshot, int slave_index)
{
   const GuiSlaveSnapshot *slave;
   const ECAT_SlaveInfo *s;
   const unsigned char *outputs;
   const unsigned char *inputs;
   int output_size;
   int input_size;
   char hex[4096];
   size_t used = 0;

   slave = snapshot_find_slave(snapshot, slave_index);
   if (slave == NULL)
   {
      safe_copy(dst, dst_size, "Select a slave module from the left list.\r\n");
      return;
   }

   s = &slave->info;
   outputs = slave->outputs;
   inputs = slave->inputs;
   output_size = slave->output_size;
   input_size = slave->input_size;

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "Slave %d - %s\r\nState: %s (0x%04X), AL: 0x%04X\r\n"
                            "Config address: 0x%04X, Alias: 0x%04X\r\n"
                            "Mailbox: W=%u B, R=%u B, Proto=0x%04X\r\n"
                            "XML DB: %s%s%s\r\n\r\n",
                            s->index, s->name, ECAT_StateName(s->state), s->state,
                            s->al_status, s->config_address, s->alias_address,
                            s->mailbox_write_bytes, s->mailbox_read_bytes,
                            s->mailbox_protocols,
                            s->database_matched ? s->database_name : "Not matched",
                            s->database_matched ? " | " : "",
                            s->database_matched ? s->database_xml : "");

   if (input_size >= 2)
   {
      unsigned short statusword = read_u16_le(inputs);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Input[0..1] as CiA 402 statusword: 0x%04X (%s)\r\n",
                               statusword, ECAT_Cia402StateName(statusword));
   }
   if (input_size >= 6)
   {
      int actual_position = read_i32_le(inputs + 2);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Input[2..5] as Actual position: %d\r\n",
                               actual_position);
   }
   if (output_size >= 2)
   {
      unsigned short controlword = read_u16_le(outputs);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Output[0..1] as CiA 402 controlword: 0x%04X\r\n",
                               controlword);
   }
   if (output_size >= 6)
   {
      int target_position = read_i32_le(outputs + 2);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Output[2..5] as Target position: %d\r\n",
                               target_position);
   }

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "\r\nOutputs/RxPDO snapshot (%d of %u bytes copied)\r\n",
                            output_size, s->output_bytes);
   hex_dump(hex, sizeof(hex), outputs, output_size);
   used += (size_t)snprintf(dst + used, dst_size - used, "%s", hex);

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "\r\nInputs/TxPDO snapshot (%d of %u bytes copied)\r\n",
                            input_size, s->input_bytes);
   hex_dump(hex, sizeof(hex), inputs, input_size);
   (void)snprintf(dst + used, dst_size - used, "%s", hex);
}

static void format_stats_text(char *dst, size_t dst_size,
                              const ECAT_RuntimeStatus *r)
{
   (void)snprintf(
      dst, dst_size,
      "Connection\r\n"
      "  State              : %s\r\n"
      "  Connected          : %s\r\n"
      "  Operational        : %s\r\n"
      "  Slaves             : %d\r\n"
      "  Last error         : %s\r\n\r\n"
      "Process Data Cycle\r\n"
      "  Requested period   : %d us\r\n"
      "  Last roundtrip     : %d us\r\n"
      "  Min roundtrip      : %d us\r\n"
      "  Max roundtrip      : %d us\r\n"
      "  Average roundtrip  : %.1f us\r\n"
      "  Total cycles       : %d\r\n"
      "  DC time            : %lld ns\r\n\r\n"
      "Working Counter / Link Health\r\n"
      "  Expected WKC       : %d\r\n"
      "  Last WKC           : %d\r\n"
      "  WKC errors         : %d\r\n"
      "  State recoveries   : %d\r\n\r\n"
      "CRC / FCS\r\n"
      "  Status             : %s\r\n",
      r->state_text, r->connected ? "Yes" : "No",
      r->operational ? "Yes" : "No", r->slave_count,
      r->last_error[0] ? r->last_error : "-",
      r->period_us, r->cycle_us, r->min_cycle_us, r->max_cycle_us,
      r->avg_cycle_us, r->total_cycles, r->dc_time_ns,
      r->expected_wkc, r->last_wkc, r->wkc_errors, r->state_errors,
      r->crc_status);
}

static int get_edit_i32(HWND edit, int fallback)
{
   char text[64];
   char *end = NULL;
   long value;

   GetWindowTextA(edit, text, sizeof(text));
   value = strtol(text, &end, 0);
   if (end == text)
   {
      return fallback;
   }
   return (int)value;
}

static unsigned int get_edit_u32(HWND edit, unsigned int fallback)
{
   int value = get_edit_i32(edit, (int)fallback);
   return value > 0 ? (unsigned int)value : fallback;
}

static int get_motion_slave(void)
{
   int slave = get_edit_i32(G_gui.motion_slave, G_gui.selected_slave);

   if (slave <= 0)
   {
      slave = G_gui.selected_slave > 0 ? G_gui.selected_slave : 1;
      SetDlgItemInt(G_gui.hwnd, IDC_MOTION_SLAVE, (UINT)slave, FALSE);
   }
   return slave;
}

static const char *motion_profile_name(int profile_type)
{
   switch (profile_type)
   {
   case ECAT_PROFILE_LMS:
      return "0 LMS";
   case ECAT_PROFILE_SCURVE:
      return "2 SCurve";
   case ECAT_PROFILE_JERK_RATIO:
      return "3 JerkRatio";
   case ECAT_PROFILE_TRAPEZOIDAL:
   default:
      return "1 Trapezoidal";
   }
}

static int get_motion_profile_type(void)
{
   int selected =
      (int)SendMessageA(G_gui.motion_profile, CB_GETCURSEL, 0, 0);

   switch (selected)
   {
   case 0:
      return ECAT_PROFILE_LMS;
   case 2:
      return ECAT_PROFILE_SCURVE;
   case 3:
      return ECAT_PROFILE_JERK_RATIO;
   case 1:
   default:
      return ECAT_PROFILE_TRAPEZOIDAL;
   }
}

static double get_motion_profile_jerk_ratio(int profile_type)
{
   switch (profile_type)
   {
   case ECAT_PROFILE_LMS:
      return 0.35;
   case ECAT_PROFILE_SCURVE:
      return 1.0;
   case ECAT_PROFILE_JERK_RATIO:
      return 0.75;
   case ECAT_PROFILE_TRAPEZOIDAL:
   default:
      return 0.0;
   }
}

static int apply_motion_profile_setting(void)
{
   int profile_type = get_motion_profile_type();
   double jerk_ratio = get_motion_profile_jerk_ratio(profile_type);
   return ECAT_SetMotionProfile(profile_type, jerk_ratio);
}

static void set_i32_text(HWND hwnd, int value)
{
   char text[64];
   (void)snprintf(text, sizeof(text), "%d", value);
   SetWindowTextA(hwnd, text);
}

static void format_motion_status(char *dst, size_t dst_size,
                                 const GuiSnapshot *snapshot)
{
   int slave = get_motion_slave();
   const GuiSlaveSnapshot *slave_snapshot;
   unsigned short statusword = 0;
   unsigned short controlword = 0;
   signed char mode_display = 0;
   int actual_position = 0;
   int actual_velocity = 0;
   int command_position = 0;
   int command_velocity = 0;
   int profile_type = ECAT_PROFILE_TRAPEZOIDAL;
   double jerk_ratio = 0.0;

   if (snapshot == NULL || !snapshot->is_open)
   {
      safe_copy(dst, dst_size, "Motion backend is disconnected.\r\n");
      SetWindowTextA(G_gui.motion_command_pos, "");
      SetWindowTextA(G_gui.motion_actual_pos, "");
      SetWindowTextA(G_gui.motion_command_vel, "");
      SetWindowTextA(G_gui.motion_actual_vel, "");
      SetWindowTextA(G_gui.motion_op_status, "Disconnected");
      return;
   }

   slave_snapshot = snapshot_find_slave(snapshot, slave);
   if (slave_snapshot == NULL)
   {
      (void)snprintf(dst, dst_size,
                     "Servo status is not available.\r\n"
                     "Slave: %d\r\n",
                     slave);
      return;
   }

   if (slave_snapshot->input_size >= 2)
   {
      statusword = read_u16_le(slave_snapshot->inputs);
   }
   if (slave_snapshot->input_size >= 6)
   {
      actual_position = read_i32_le(slave_snapshot->inputs + 2);
   }
   if (slave_snapshot->input_size >= 10)
   {
      actual_velocity = read_i32_le(slave_snapshot->inputs + 6);
   }
   if (slave_snapshot->input_size >= 11)
   {
      mode_display = (signed char)slave_snapshot->inputs[10];
   }
   if (slave_snapshot->output_size >= 2)
   {
      controlword = read_u16_le(slave_snapshot->outputs);
   }
   if (slave_snapshot->output_size >= 6)
   {
      command_position = read_i32_le(slave_snapshot->outputs + 2);
   }
   if (slave_snapshot->output_size >= 10)
   {
      command_velocity = read_i32_le(slave_snapshot->outputs + 6);
   }

   set_i32_text(G_gui.motion_command_pos, command_position);
   set_i32_text(G_gui.motion_actual_pos, actual_position);
   set_i32_text(G_gui.motion_command_vel, command_velocity);
   set_i32_text(G_gui.motion_actual_vel, actual_velocity);
   SetWindowTextA(G_gui.motion_op_status, ECAT_Cia402StateName(statusword));

   (void)ECAT_GetMotionProfile(&profile_type, &jerk_ratio);
   (void)snprintf(dst, dst_size,
                  "Selected slave\r\n"
                  "  Slave              : %d\r\n\r\n"
                  "CiA402 Status\r\n"
                  "  State              : %s\r\n"
                  "  Statusword         : 0x%04X\r\n"
                  "  Controlword        : 0x%04X\r\n"
                  "  Mode display       : %d\r\n"
                  "  Command position   : %d\r\n"
                  "  Actual position    : %d\r\n"
                  "  Command velocity   : %d\r\n"
                  "  Actual velocity    : %d\r\n"
                  "  Error code         : 0x%04X\r\n"
                  "  Target reached     : %s\r\n"
                  "  Fault              : %s\r\n"
                  "  Warning            : %s\r\n\r\n"
                  "Command fields\r\n"
                  "  Position1          : %d\r\n"
                  "  Position2          : %d\r\n"
                  "  Velocity/Jog       : %d\r\n"
                  "  Acceleration       : %u\r\n"
                  "  Deceleration       : %u\r\n"
                  "  Homing method      : %d\r\n"
                  "  Repeat delay       : %d ms\r\n"
                  "  Profile type       : %s\r\n"
                  "  Jerk ratio         : %.2f\r\n",
                  slave,
                  ECAT_Cia402StateName(statusword),
                  statusword,
                  controlword,
                  mode_display,
                  command_position,
                  actual_position,
                  command_velocity,
                  actual_velocity,
                  0,
                  (statusword & 0x0400U) ? "Yes" : "No",
                  (statusword & 0x0008U) ? "Yes" : "No",
                  (statusword & 0x0080U) ? "Yes" : "No",
                  get_edit_i32(G_gui.motion_target, 0),
                  get_edit_i32(G_gui.motion_position2, 0),
                  get_edit_i32(G_gui.motion_velocity, 0),
                  get_edit_u32(G_gui.motion_accel, 0),
                  get_edit_u32(G_gui.motion_decel, 0),
                  get_edit_i32(G_gui.motion_home_method, 35),
                  get_edit_i32(G_gui.motion_delay, 0),
                  motion_profile_name(profile_type),
                  jerk_ratio);
}

static void finish_motion_command(const char *name, int result)
{
   char message[512];

   (void)snprintf(message, sizeof(message), "%s: %s (%d)",
                  name,
                  result == ECAT_OK ? "OK" : ECAT_ErrorToString(result),
                  result);
   append_log_line(result == ECAT_OK ? "INFO" : "WARN", message);
   if (result != ECAT_OK)
   {
      MessageBoxA(G_gui.hwnd, message, APP_TITLE, MB_ICONWARNING | MB_OK);
   }
}

static void interruptible_sleep_ms(int total_ms)
{
   int elapsed = 0;
   if (total_ms < 0)
   {
      total_ms = 0;
   }
   while (elapsed < total_ms &&
          InterlockedCompareExchange(&G_gui.repeat_stop, 0, 0) == 0)
   {
      int step = total_ms - elapsed;
      if (step > 50)
      {
         step = 50;
      }
      Sleep((DWORD)step);
      elapsed += step;
   }
}

static void wait_repeat_in_position(int slave, int target_position)
{
   int elapsed = 0;

   while (elapsed < 30000 &&
          InterlockedCompareExchange(&G_gui.repeat_stop, 0, 0) == 0)
   {
      ECAT_ServoStatus status;
      memset(&status, 0, sizeof(status));
      if (ECAT_ServoGetStatus(slave, &status) == ECAT_OK)
      {
         int error = status.actual_position - target_position;
         if (status.target_reached || abs(error) <= 10)
         {
            return;
         }
      }
      Sleep(50);
      elapsed += 50;
   }
}

static DWORD WINAPI repeat_thread_proc(LPVOID arg)
{
   MotionRepeatArgs *repeat = (MotionRepeatArgs *)arg;
   int result = ECAT_OK;

   if (repeat == NULL)
   {
      InterlockedExchange(&G_gui.repeat_running, 0);
      return 0;
   }

   append_log_line("INFO", "Repeat motion started");
   while (InterlockedCompareExchange(&G_gui.repeat_stop, 0, 0) == 0)
   {
      result = ECAT_ServoMoveAbs(repeat->slave,
                                 repeat->position1,
                                 repeat->velocity,
                                 repeat->acceleration,
                                 repeat->deceleration);
      if (result != ECAT_OK)
      {
         finish_motion_command("Repeat AbsMove1", result);
         break;
      }
      if (repeat->check_inpos)
      {
         wait_repeat_in_position(repeat->slave, repeat->position1);
      }
      interruptible_sleep_ms(repeat->delay_ms);
      if (InterlockedCompareExchange(&G_gui.repeat_stop, 0, 0) != 0)
      {
         break;
      }

      result = ECAT_ServoMoveAbs(repeat->slave,
                                 repeat->position2,
                                 repeat->velocity,
                                 repeat->acceleration,
                                 repeat->deceleration);
      if (result != ECAT_OK)
      {
         finish_motion_command("Repeat AbsMove2", result);
         break;
      }
      if (repeat->check_inpos)
      {
         wait_repeat_in_position(repeat->slave, repeat->position2);
      }
      interruptible_sleep_ms(repeat->delay_ms);
   }

   HeapFree(GetProcessHeap(), 0, repeat);
   InterlockedExchange(&G_gui.repeat_running, 0);
   InterlockedExchange(&G_gui.repeat_stop, 0);
   append_log_line("INFO", "Repeat motion stopped");
   if (G_gui.hwnd != NULL)
   {
      PostMessageA(G_gui.hwnd, WM_GUI_STATUS_READY, 0, 0);
   }
   return 0;
}

static void stop_repeat_motion(int wait_for_stop)
{
   InterlockedExchange(&G_gui.repeat_stop, 1);
   if (wait_for_stop && G_gui.repeat_thread != NULL)
   {
      WaitForSingleObject(G_gui.repeat_thread, 2000);
      CloseHandle(G_gui.repeat_thread);
      G_gui.repeat_thread = NULL;
   }
}

static void start_repeat_motion(void)
{
   MotionRepeatArgs *repeat;
   int result;

   if (!ECAT_IsOpen())
   {
      MessageBoxA(G_gui.hwnd, "Connect the EtherCAT backend first.",
                  APP_TITLE, MB_ICONWARNING | MB_OK);
      return;
   }
   if (InterlockedCompareExchange(&G_gui.repeat_running, 1, 0) != 0)
   {
      return;
   }

   result = apply_motion_profile_setting();
   if (result != ECAT_OK)
   {
      InterlockedExchange(&G_gui.repeat_running, 0);
      finish_motion_command("Set Motion Profile", result);
      return;
   }

   repeat = (MotionRepeatArgs *)HeapAlloc(GetProcessHeap(),
                                          HEAP_ZERO_MEMORY,
                                          sizeof(*repeat));
   if (repeat == NULL)
   {
      InterlockedExchange(&G_gui.repeat_running, 0);
      append_log_line("ERROR", "Repeat motion allocation failed");
      return;
   }

   repeat->slave = get_motion_slave();
   repeat->position1 = get_edit_i32(G_gui.motion_target, 0);
   repeat->position2 = get_edit_i32(G_gui.motion_position2, 0);
   repeat->velocity = (unsigned int)abs(get_edit_i32(G_gui.motion_velocity, 0));
   repeat->acceleration = get_edit_u32(G_gui.motion_accel, 0);
   repeat->deceleration = get_edit_u32(G_gui.motion_decel, 0);
   repeat->delay_ms = get_edit_i32(G_gui.motion_delay, 0);
   repeat->check_inpos =
      SendMessageA(G_gui.motion_check_inpos, BM_GETCHECK, 0, 0) == BST_CHECKED;
   if (repeat->delay_ms < 0)
   {
      repeat->delay_ms = 0;
   }

   InterlockedExchange(&G_gui.repeat_stop, 0);
   if (G_gui.repeat_thread != NULL)
   {
      CloseHandle(G_gui.repeat_thread);
      G_gui.repeat_thread = NULL;
   }
   G_gui.repeat_thread = CreateThread(NULL, 0, repeat_thread_proc,
                                      repeat, 0, NULL);
   if (G_gui.repeat_thread == NULL)
   {
      HeapFree(GetProcessHeap(), 0, repeat);
      InterlockedExchange(&G_gui.repeat_running, 0);
      append_log_line("ERROR", "Repeat motion thread creation failed");
   }
}

static void request_motion_command(int control_id)
{
   int slave = get_motion_slave();
   int target = get_edit_i32(G_gui.motion_target, 0);
   int target2 = get_edit_i32(G_gui.motion_position2, 0);
   int velocity = get_edit_i32(G_gui.motion_velocity, 0);
   unsigned int accel = get_edit_u32(G_gui.motion_accel, 0);
   unsigned int decel = get_edit_u32(G_gui.motion_decel, 0);
   int home_method = get_edit_i32(G_gui.motion_home_method, 35);
   int result = ECAT_ERROR;
   const char *name = "Motion command";

   if (!ECAT_IsOpen())
   {
      MessageBoxA(G_gui.hwnd, "Connect the EtherCAT backend first.",
                  APP_TITLE, MB_ICONWARNING | MB_OK);
      return;
   }
   result = apply_motion_profile_setting();
   if (result != ECAT_OK)
   {
      finish_motion_command("Set Motion Profile", result);
      return;
   }

   switch (control_id)
   {
   case IDC_MOTION_FAULT_RESET:
      name = "Servo Fault Reset";
      result = ECAT_ServoFaultReset(slave);
      break;
   case IDC_MOTION_ENABLE:
      name = "Servo Enable";
      result = ECAT_ServoEnable(slave);
      break;
   case IDC_MOTION_DISABLE:
      name = "Servo Disable";
      result = ECAT_ServoDisable(slave);
      break;
   case IDC_MOTION_STOP:
      stop_repeat_motion(0);
      name = "Servo Stop";
      result = ECAT_ServoStop(slave);
      break;
   case IDC_MOTION_JOG_POS:
      name = "Jog +";
      if (velocity < 0)
      {
         velocity = -velocity;
      }
      result = ECAT_ServoJog(slave, velocity, accel, decel);
      break;
   case IDC_MOTION_JOG_NEG:
      name = "Jog -";
      if (velocity > 0)
      {
         velocity = -velocity;
      }
      result = ECAT_ServoJog(slave, velocity, accel, decel);
      break;
   case IDC_MOTION_MOVE_ABS:
      name = "Move Abs";
      result = ECAT_ServoMoveAbs(slave, target, (unsigned int)abs(velocity),
                                 accel, decel);
      break;
   case IDC_MOTION_MOVE_ABS2:
      name = "AbsMove2";
      result = ECAT_ServoMoveAbs(slave, target2, (unsigned int)abs(velocity),
                                 accel, decel);
      break;
   case IDC_MOTION_MOVE_REL:
      name = "Move Rel";
      result = ECAT_ServoMoveRel(slave, target, (unsigned int)abs(velocity),
                                 accel, decel);
      break;
   case IDC_MOTION_HOME:
      name = "Home";
      result = ECAT_ServoHome(slave, (signed char)home_method,
                              (unsigned int)abs(velocity), 0, accel);
      break;
   default:
      break;
   }

   finish_motion_command(name, result);
}

static void collect_gui_snapshot(GuiSnapshot *snapshot)
{
   int count;
   int i;

   if (snapshot == NULL)
   {
      return;
   }

   memset(snapshot, 0, sizeof(*snapshot));
   snapshot->is_open = ECAT_IsOpen();
   (void)ECAT_GetRuntimeStatus(&snapshot->runtime);
   snapshot->is_open = ECAT_IsOpen();

   count = snapshot->runtime.slave_count;
   if (count > GUI_MAX_SLAVES)
   {
      count = GUI_MAX_SLAVES;
   }
   for (i = 1; i <= count; ++i)
   {
      GuiSlaveSnapshot *slave = &snapshot->slaves[i - 1];
      if (ECAT_GetSlaveInfo(i, &slave->info) != ECAT_OK)
      {
         continue;
      }
      (void)ECAT_GetPdoSnapshot(i,
                                slave->outputs,
                                sizeof(slave->outputs),
                                &slave->output_size,
                                slave->inputs,
                                sizeof(slave->inputs),
                                &slave->input_size);
      slave->valid = 1;
   }
}

static DWORD WINAPI poll_thread_proc(LPVOID arg)
{
   (void)arg;

   while (InterlockedCompareExchange(&G_gui.poll_stop, 0, 0) == 0)
   {
      GuiSnapshot snapshot;
      collect_gui_snapshot(&snapshot);

      EnterCriticalSection(&G_gui.snapshot_lock);
      G_gui.snapshot = snapshot;
      LeaveCriticalSection(&G_gui.snapshot_lock);

      if (G_gui.hwnd != NULL &&
          InterlockedExchange(&G_gui.update_pending, 1) == 0)
      {
         PostMessageA(G_gui.hwnd, WM_GUI_STATUS_READY, 0, 0);
      }
      Sleep(UPDATE_TIMER_MS);
   }

   return 0;
}

static void start_poll_thread(void)
{
   if (G_gui.poll_thread != NULL)
   {
      return;
   }
   InterlockedExchange(&G_gui.poll_stop, 0);
   G_gui.poll_thread = CreateThread(NULL, 0, poll_thread_proc, NULL, 0, NULL);
   if (G_gui.poll_thread == NULL)
   {
      append_log_line("ERROR", "GUI polling thread creation failed");
   }
}

static void stop_poll_thread(void)
{
   InterlockedExchange(&G_gui.poll_stop, 1);
   if (G_gui.poll_thread != NULL)
   {
      WaitForSingleObject(G_gui.poll_thread, 2000);
      CloseHandle(G_gui.poll_thread);
      G_gui.poll_thread = NULL;
   }
}

static void update_gui_from_snapshot(void)
{
   GuiSnapshot snapshot;
   char summary[512];
   char text[32768];
   char logs[MAX_GUI_LOG];
   DWORD now;

   EnterCriticalSection(&G_gui.snapshot_lock);
   snapshot = G_gui.snapshot;
   LeaveCriticalSection(&G_gui.snapshot_lock);

   (void)snprintf(summary, sizeof(summary),
                  "%s | Slaves: %d | WKC: %d/%d | Cycle: %d us "
                  "(avg %.1f us, min %d, max %d) | WKC errors: %d",
                  snapshot.runtime.state_text, snapshot.runtime.slave_count,
                  snapshot.runtime.last_wkc, snapshot.runtime.expected_wkc,
                  snapshot.runtime.cycle_us, snapshot.runtime.avg_cycle_us,
                  snapshot.runtime.min_cycle_us, snapshot.runtime.max_cycle_us,
                  snapshot.runtime.wkc_errors);
   SetWindowTextA(G_gui.summary, summary);

   update_list_views(&snapshot);

   format_pdo_text(text, sizeof(text), &snapshot, G_gui.selected_slave);
   SetWindowTextA(G_gui.pdo_edit, text);

   format_stats_text(text, sizeof(text), &snapshot.runtime);
   SetWindowTextA(G_gui.stats_edit, text);

   if (G_gui.active_tab == 4)
   {
      format_motion_status(text, sizeof(text), &snapshot);
      SetWindowTextA(G_gui.motion_status, text);
   }

   now = GetTickCount();
   if (G_gui.active_tab == 3 &&
       SendMessageA(G_gui.coe_auto_update, BM_GETCHECK, 0, 0) == BST_CHECKED &&
       snapshot.is_open &&
       InterlockedCompareExchange(&G_gui.coe_running, 0, 0) == 0 &&
       (G_gui.last_coe_auto_update_tick == 0 ||
        now - G_gui.last_coe_auto_update_tick >= GUI_COE_AUTO_UPDATE_MS))
   {
      G_gui.last_coe_auto_update_tick = now;
      PostMessageA(G_gui.hwnd, WM_COMMAND,
                   MAKEWPARAM(IDC_COE_UPDATE, BN_CLICKED),
                   (LPARAM)G_gui.coe_update);
   }

   EnterCriticalSection(&G_gui.log_lock);
   safe_copy(logs, sizeof(logs), G_gui.log_text);
   LeaveCriticalSection(&G_gui.log_lock);
   SetWindowTextA(G_gui.log_edit, logs);
   SendMessageA(G_gui.log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
   SendMessageA(G_gui.log_edit, EM_SCROLLCARET, 0, 0);

   EnableWindow(G_gui.connect, !snapshot.is_open);
   EnableWindow(G_gui.disconnect, snapshot.is_open);
   EnableWindow(G_gui.sdo_read, snapshot.is_open);
   EnableWindow(G_gui.coe_update, snapshot.is_open &&
                              InterlockedCompareExchange(&G_gui.coe_running, 0, 0) == 0);
   EnableWindow(G_gui.motion_fault_reset, snapshot.is_open);
   EnableWindow(G_gui.motion_enable, snapshot.is_open);
   EnableWindow(G_gui.motion_disable, snapshot.is_open);
   EnableWindow(G_gui.motion_stop, snapshot.is_open);
   EnableWindow(G_gui.motion_jog_pos, snapshot.is_open);
   EnableWindow(G_gui.motion_jog_neg, snapshot.is_open);
   EnableWindow(G_gui.motion_move_abs, snapshot.is_open);
   EnableWindow(G_gui.motion_move_abs2, snapshot.is_open);
   EnableWindow(G_gui.motion_move_rel, snapshot.is_open);
   EnableWindow(G_gui.motion_home, snapshot.is_open);
   EnableWindow(G_gui.motion_repeat, snapshot.is_open &&
                                   InterlockedCompareExchange(&G_gui.repeat_running, 0, 0) == 0);
}

static void start_master(void)
{
   char adapter[ECAT_MAX_ADAPTER_NAME];
   char period_text[32];
   ECAT_OpenOptions options;
   int period_us;
   int result;
   int backend = ECAT_BACKEND_WINDOWS_DEBUG;

   result = apply_backend_settings();
   if (result != ECAT_OK)
   {
      MessageBoxA(G_gui.hwnd, "Backend setting failed.", APP_TITLE,
                  MB_ICONERROR | MB_OK);
      return;
   }
   ECAT_GetBackend(&backend);

   if (backend == ECAT_BACKEND_WINDOWS_DEBUG &&
       SendMessageA(G_gui.adapter, CB_GETCURSEL, 0, 0) == CB_ERR)
   {
      MessageBoxA(G_gui.hwnd, "No adapter selected.", APP_TITLE,
                  MB_ICONWARNING | MB_OK);
      return;
   }

   GetWindowTextA(G_gui.adapter, adapter, sizeof(adapter));
   if (backend == ECAT_BACKEND_LINUX_RT)
   {
      safe_copy(adapter, sizeof(adapter), "Linux RT Controller");
   }
   GetWindowTextA(G_gui.period, period_text, sizeof(period_text));
   period_us = atoi(period_text);
   if (period_us <= 0)
   {
      period_us = DEFAULT_PERIOD_US;
      SetWindowTextA(G_gui.period, "1000");
   }

   memset(&options, 0, sizeof(options));
   options.period_us = period_us;
   options.request_operational =
      (SendMessageA(G_gui.opmode, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

   result = ECAT_Open(adapter, &options);
   if (result != ECAT_OK)
   {
      char message[512];
      char detail[ECAT_MAX_MESSAGE];
      ECAT_GetLastError(detail, sizeof(detail));
      (void)snprintf(message, sizeof(message), "ECAT_Open failed.\r\n%s\r\n%s",
                     ECAT_ErrorToString(result), detail);
      MessageBoxA(G_gui.hwnd, message, APP_TITLE,
                  MB_ICONERROR | MB_OK);
   }
}

static void request_sdo_read(void)
{
   unsigned char data[256];
   int data_size = 0;
   int slave = GetDlgItemInt(G_gui.hwnd, IDC_SDO_SLAVE, NULL, FALSE);
   int size = GetDlgItemInt(G_gui.hwnd, IDC_SDO_SIZE, NULL, FALSE);
   unsigned long index;
   unsigned long subindex;
   char text[64];
   char result_text[4096];
   char hex[2048];
   int result;

   GetWindowTextA(G_gui.sdo_index, text, sizeof(text));
   index = strtoul(text, NULL, 0);
   GetWindowTextA(G_gui.sdo_sub, text, sizeof(text));
   subindex = strtoul(text, NULL, 0);
   if (size <= 0 || size > (int)sizeof(data))
   {
      size = 64;
      SetWindowTextA(G_gui.sdo_size, "64");
   }

   result = ECAT_ReadSdo(slave, (unsigned short)index,
                         (unsigned char)subindex, data, size, &data_size);
   hex_dump(hex, sizeof(hex), data, data_size);
   (void)snprintf(result_text, sizeof(result_text),
                  "SDO read slave=%d index=0x%04lX sub=0x%02lX\r\n"
                  "Result: %s (%d)\r\nData length: %d\r\n\r\n%s",
                  slave, index, subindex,
                  result == ECAT_OK ? "OK" : ECAT_ErrorToString(result),
                  result, data_size, hex);
   SetWindowTextA(G_gui.sdo_result, result_text);
   append_log_line(result == ECAT_OK ? "INFO" : "WARN", result_text);
}

typedef struct CoeObjectDef
{
   unsigned short index;
   unsigned char subindex;
   unsigned char size;
   const char *name;
   const char *flags;
   int signed_value;
} CoeObjectDef;

static const CoeObjectDef G_coe_objects[] = {
   {0x1000, 0x00, 4, "Device type", "RO", 0},
   {0x1001, 0x00, 1, "Error register", "RO", 0},
   {0x1008, 0x00, 64, "Device name", "RO", 0},
   {0x1009, 0x00, 64, "Manufacturer hardware version", "RO", 0},
   {0x100A, 0x00, 64, "Manufacturer software version", "RO", 0},
   {0x1018, 0x00, 1, "Identity object: entries", "RO", 0},
   {0x1018, 0x01, 4, "Vendor ID", "RO", 0},
   {0x1018, 0x02, 4, "Product code", "RO", 0},
   {0x1018, 0x03, 4, "Revision", "RO", 0},
   {0x1018, 0x04, 4, "Serial number", "RO", 0},
   {0x1600, 0x00, 1, "RxPDO mapping CSP/CSV: entries", "RO", 0},
   {0x1600, 0x01, 4, "RxPDO 1: Controlword", "RO", 0},
   {0x1600, 0x02, 4, "RxPDO 2: Target position", "RO", 0},
   {0x1600, 0x03, 4, "RxPDO 3: Target velocity", "RO", 0},
   {0x1600, 0x04, 4, "RxPDO 4: Modes of operation", "RO", 0},
   {0x1600, 0x05, 4, "RxPDO 5: Gap", "RO", 0},
   {0x1A00, 0x00, 1, "TxPDO mapping CSP/CSV: entries", "RO", 0},
   {0x1A00, 0x01, 4, "TxPDO 1: Statusword", "RO", 0},
   {0x1A00, 0x02, 4, "TxPDO 2: Position actual value", "RO", 0},
   {0x1A00, 0x03, 4, "TxPDO 3: Velocity actual value", "RO", 0},
   {0x1A00, 0x04, 4, "TxPDO 4: Modes of operation display", "RO", 0},
   {0x1A00, 0x05, 4, "TxPDO 5: Gap", "RO", 0},
   {0x1C00, 0x00, 1, "Sync manager type: entries", "RO", 0},
   {0x1C12, 0x00, 1, "SM2 assignment: entries", "RO", 0},
   {0x1C12, 0x01, 2, "SM2 assigned RxPDO", "RO", 0},
   {0x1C13, 0x00, 1, "SM3 assignment: entries", "RO", 0},
   {0x1C13, 0x01, 2, "SM3 assigned TxPDO", "RO", 0},
   {0x3000, 0x00, 4, "PosKpGain", "RW", 1},
   {0x3001, 0x00, 4, "PosKpDivisor", "RO", 1},
   {0x3002, 0x00, 4, "PosKiGain", "RW", 1},
   {0x3003, 0x00, 4, "PosKiDivisor", "RO", 1},
   {0x3004, 0x00, 4, "PosKdGain", "RW", 1},
   {0x603F, 0x00, 2, "Error code", "RO", 0},
   {0x6040, 0x00, 2, "Controlword", "RW", 0},
   {0x6041, 0x00, 2, "Statusword", "RO", 0},
   {0x6060, 0x00, 1, "Modes of operation", "RW", 1},
   {0x6061, 0x00, 1, "Modes of operation display", "RO", 1},
   {0x6064, 0x00, 4, "Position actual value", "RO", 1},
   {0x606C, 0x00, 4, "Velocity actual value", "RO", 1},
   {0x607A, 0x00, 4, "Target position", "RW", 1},
   {0x60FF, 0x00, 4, "Target velocity", "RW", 1},
   {0x6502, 0x00, 4, "Supported drive modes", "RO", 0},
};

static int is_printable_data(const unsigned char *data, int size)
{
   int i;
   int printable = 0;

   if (data == NULL || size <= 0)
   {
      return 0;
   }
   for (i = 0; i < size; ++i)
   {
      if (data[i] == 0)
      {
         continue;
      }
      if (data[i] < 32 || data[i] > 126)
      {
         return 0;
      }
      printable = 1;
   }
   return printable;
}

static void format_sdo_value(char *dst, size_t dst_size,
                             const CoeObjectDef *def,
                             const unsigned char *data, int size,
                             int result)
{
   unsigned int raw = 0;
   char ascii[80];
   int i;

   if (result != ECAT_OK || data == NULL || size <= 0)
   {
      safe_copy(dst, dst_size, "Offline / unsupported");
      return;
   }

   if (is_printable_data(data, size))
   {
      int copy = size;
      if (copy >= (int)sizeof(ascii))
      {
         copy = (int)sizeof(ascii) - 1;
      }
      memcpy(ascii, data, (size_t)copy);
      ascii[copy] = '\0';
      (void)snprintf(dst, dst_size, "%s", ascii);
      return;
   }

   for (i = 0; i < size && i < 4; ++i)
   {
      raw |= ((unsigned int)data[i]) << (8 * i);
   }

   if (size == 1)
   {
      if (def->signed_value)
      {
         (void)snprintf(dst, dst_size, "0x%02X (%d)",
                        raw & 0xffU, (signed char)(raw & 0xffU));
      }
      else
      {
         (void)snprintf(dst, dst_size, "0x%02X (%u)",
                        raw & 0xffU, raw & 0xffU);
      }
   }
   else if (size == 2)
   {
      (void)snprintf(dst, dst_size, "0x%04X (%u)",
                     raw & 0xffffU, raw & 0xffffU);
   }
   else if (size == 4)
   {
      if (def->signed_value)
      {
         (void)snprintf(dst, dst_size, "0x%08X (%d)", raw, (int)(int32_t)raw);
      }
      else
      {
         (void)snprintf(dst, dst_size, "0x%08X (%u)", raw, raw);
      }
   }
   else
   {
      char hex[512];
      hex_dump(hex, sizeof(hex), data, size);
      safe_copy(dst, dst_size, hex);
   }
}

static DWORD WINAPI coe_thread_proc(LPVOID arg)
{
   CoeThreadArgs *args = (CoeThreadArgs *)arg;
   int slave = args != NULL ? args->slave : 1;
   int show_offline = args != NULL ? args->show_offline : 1;
   GuiCoeSnapshot snapshot;
   int i;

   memset(&snapshot, 0, sizeof(snapshot));
   snapshot.slave = slave;
   snapshot.result = ECAT_OK;

   for (i = 0; i < (int)(sizeof(G_coe_objects) / sizeof(G_coe_objects[0])) &&
               snapshot.count < GUI_MAX_COE_ENTRIES; ++i)
   {
      const CoeObjectDef *def = &G_coe_objects[i];
      unsigned char data[256];
      int data_size = 0;
      int result;
      GuiCoeEntry *entry;

      memset(data, 0, sizeof(data));
      result = ECAT_ReadSdo(slave, def->index, def->subindex,
                            data, def->size, &data_size);
      if (result != ECAT_OK && !show_offline)
      {
         continue;
      }

      entry = &snapshot.entries[snapshot.count++];
      entry->index = def->index;
      entry->subindex = def->subindex;
      entry->online = result == ECAT_OK;
      entry->size = data_size;
      safe_copy(entry->name, sizeof(entry->name), def->name);
      safe_copy(entry->flags, sizeof(entry->flags), def->flags);
      format_sdo_value(entry->value, sizeof(entry->value), def,
                       data, data_size, result);
   }

   (void)snprintf(snapshot.detail, sizeof(snapshot.detail),
                  "CoE Online scan\r\n"
                  "  Slave        : %d\r\n"
                  "  Entries      : %d\r\n"
                  "  Source       : Online SDO reads with offline fallback list\r\n",
                  snapshot.slave, snapshot.count);

   EnterCriticalSection(&G_gui.coe_lock);
   G_gui.coe_snapshot = snapshot;
   LeaveCriticalSection(&G_gui.coe_lock);
   InterlockedExchange(&G_gui.coe_running, 0);
   if (G_gui.hwnd != NULL)
   {
      PostMessageA(G_gui.hwnd, WM_GUI_COE_READY, 0, 0);
   }
   if (args != NULL)
   {
      HeapFree(GetProcessHeap(), 0, args);
   }
   return 0;
}

static void update_coe_list(void)
{
   GuiCoeSnapshot snapshot;
   int i;

   EnterCriticalSection(&G_gui.coe_lock);
   snapshot = G_gui.coe_snapshot;
   LeaveCriticalSection(&G_gui.coe_lock);

   SendMessageA(G_gui.coe_list, WM_SETREDRAW, FALSE, 0);
   ListView_DeleteAllItems(G_gui.coe_list);
   for (i = 0; i < snapshot.count; ++i)
   {
      const GuiCoeEntry *entry = &snapshot.entries[i];
      char text[64];

      (void)snprintf(text, sizeof(text), "0x%04X", entry->index);
      insert_row(G_gui.coe_list, i, text);
      (void)snprintf(text, sizeof(text), "0x%02X", entry->subindex);
      set_row_text(G_gui.coe_list, i, 1, text);
      set_row_text(G_gui.coe_list, i, 2, entry->name);
      set_row_text(G_gui.coe_list, i, 3, entry->flags);
      set_row_text(G_gui.coe_list, i, 4, entry->value);
      set_row_text(G_gui.coe_list, i, 5, entry->online ? "Yes" : "No");
   }
   SendMessageA(G_gui.coe_list, WM_SETREDRAW, TRUE, 0);
   InvalidateRect(G_gui.coe_list, NULL, TRUE);
   SetWindowTextA(G_gui.coe_detail, snapshot.detail);
}

static void start_coe_update(void)
{
   int slave = get_edit_i32(G_gui.coe_slave, G_gui.selected_slave);
   CoeThreadArgs *args;

   if (slave <= 0)
   {
      slave = G_gui.selected_slave > 0 ? G_gui.selected_slave : 1;
      SetDlgItemInt(G_gui.hwnd, IDC_COE_SLAVE, (UINT)slave, FALSE);
   }
   if (!ECAT_IsOpen())
   {
      MessageBoxA(G_gui.hwnd, "Connect the EtherCAT backend first.",
                  APP_TITLE, MB_ICONWARNING | MB_OK);
      return;
   }
   if (InterlockedCompareExchange(&G_gui.coe_running, 1, 0) != 0)
   {
      return;
   }
   args = (CoeThreadArgs *)HeapAlloc(GetProcessHeap(),
                                     HEAP_ZERO_MEMORY,
                                     sizeof(*args));
   if (args == NULL)
   {
      InterlockedExchange(&G_gui.coe_running, 0);
      append_log_line("ERROR", "CoE update allocation failed");
      return;
   }
   args->slave = slave;
   args->show_offline =
      SendMessageA(G_gui.coe_show_offline, BM_GETCHECK, 0, 0) == BST_CHECKED;
   if (G_gui.coe_thread != NULL)
   {
      CloseHandle(G_gui.coe_thread);
      G_gui.coe_thread = NULL;
   }
   G_gui.coe_thread = CreateThread(NULL, 0, coe_thread_proc,
                                   args, 0, NULL);
   if (G_gui.coe_thread == NULL)
   {
      HeapFree(GetProcessHeap(), 0, args);
      InterlockedExchange(&G_gui.coe_running, 0);
      append_log_line("ERROR", "CoE update thread creation failed");
   }
}

static void on_slave_selection(LPARAM lparam)
{
   LPNMLISTVIEW lv = (LPNMLISTVIEW)lparam;
   if ((lv->uChanged & LVIF_STATE) != 0 &&
       (lv->uNewState & LVIS_SELECTED) != 0)
   {
      char text[32];
      ListView_GetItemText(G_gui.slaves, lv->iItem, 0, text, sizeof(text));
      G_gui.selected_slave = atoi(text);
      if (G_gui.selected_slave <= 0)
      {
         G_gui.selected_slave = 1;
      }
      SetDlgItemInt(G_gui.hwnd, IDC_MOTION_SLAVE,
                    (UINT)G_gui.selected_slave, FALSE);
      SetDlgItemInt(G_gui.hwnd, IDC_COE_SLAVE,
                    (UINT)G_gui.selected_slave, FALSE);
   }
}

static void on_xml_selection(LPARAM lparam)
{
   LPNMLISTVIEW lv = (LPNMLISTVIEW)lparam;
   if ((lv->uChanged & LVIF_STATE) != 0 &&
       (lv->uNewState & LVIS_SELECTED) != 0)
   {
      update_xml_detail(lv->iItem);
   }
}

static INT_PTR CALLBACK MainDlgProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
   (void)lparam;

   switch (msg)
   {
   case WM_INITDIALOG:
      bind_controls(hwnd);
      layout_controls(hwnd);
      ECAT_SetLogCallback(dll_log_callback);
      (void)apply_motion_profile_setting();
      refresh_adapters();
      update_xml_list();
      start_poll_thread();
      append_log_line("INFO", "SOEM EtherCAT Master Monitor started");
      return TRUE;

   case WM_SIZE:
      layout_controls(hwnd);
      return TRUE;

   case WM_GUI_STATUS_READY:
      InterlockedExchange(&G_gui.update_pending, 0);
      update_gui_from_snapshot();
      return TRUE;

   case WM_GUI_COE_READY:
      update_coe_list();
      return TRUE;

   case WM_COMMAND:
      switch (LOWORD(wparam))
      {
      case IDC_BACKEND:
         if (HIWORD(wparam) == CBN_SELCHANGE)
         {
            refresh_adapters();
            return TRUE;
         }
         break;
      case IDC_MOTION_PROFILE:
         if (HIWORD(wparam) == CBN_SELCHANGE)
         {
            (void)apply_motion_profile_setting();
            return TRUE;
         }
         break;
      case IDC_REFRESH:
         refresh_adapters();
         return TRUE;
      case IDC_CONNECT:
         start_master();
         return TRUE;
      case IDC_DISCONNECT:
         ECAT_Close();
         append_log_line("INFO", "Disconnect requested");
         return TRUE;
      case IDC_SDO_READ:
         request_sdo_read();
         return TRUE;
      case IDC_MOTION_FAULT_RESET:
      case IDC_MOTION_ENABLE:
      case IDC_MOTION_DISABLE:
      case IDC_MOTION_STOP:
      case IDC_MOTION_JOG_POS:
      case IDC_MOTION_JOG_NEG:
      case IDC_MOTION_MOVE_ABS:
      case IDC_MOTION_MOVE_ABS2:
      case IDC_MOTION_MOVE_REL:
      case IDC_MOTION_HOME:
         request_motion_command(LOWORD(wparam));
         return TRUE;
      case IDC_MOTION_REPEAT:
         start_repeat_motion();
         return TRUE;
      case IDC_COE_UPDATE:
         start_coe_update();
         return TRUE;
      case IDC_XML_IMPORT:
         import_xml_file();
         return TRUE;
      case IDC_XML_RELOAD:
         reload_xml_db();
         return TRUE;
      default:
         break;
      }
      break;

   case WM_NOTIFY:
      if (((LPNMHDR)lparam)->idFrom == IDC_TABS &&
          ((LPNMHDR)lparam)->code == TCN_SELCHANGE)
      {
         G_gui.active_tab = TabCtrl_GetCurSel(G_gui.tabs);
         show_tab_controls(G_gui.active_tab);
         return TRUE;
      }
      if (((LPNMHDR)lparam)->idFrom == IDC_SLAVES &&
          ((LPNMHDR)lparam)->code == LVN_ITEMCHANGED)
      {
         on_slave_selection(lparam);
         return TRUE;
      }
      if (((LPNMHDR)lparam)->idFrom == IDC_XML_LIST &&
          ((LPNMHDR)lparam)->code == LVN_ITEMCHANGED)
      {
         on_xml_selection(lparam);
         return TRUE;
      }
      break;

   case WM_CLOSE:
      stop_repeat_motion(0);
      ECAT_Close();
      DestroyWindow(hwnd);
      return TRUE;

   case WM_DESTROY:
      stop_poll_thread();
      stop_repeat_motion(1);
      if (G_gui.coe_thread != NULL)
      {
         WaitForSingleObject(G_gui.coe_thread, 2000);
         CloseHandle(G_gui.coe_thread);
         G_gui.coe_thread = NULL;
      }
      PostQuitMessage(0);
      return TRUE;
   }

   return FALSE;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line,
                   int show_cmd)
{
   INITCOMMONCONTROLSEX icc;
   HWND hwnd;
   MSG msg;

   (void)prev_instance;
   (void)cmd_line;

   memset(&G_gui, 0, sizeof(G_gui));
   G_gui.selected_slave = 1;
   InitializeCriticalSection(&G_gui.log_lock);
   InitializeCriticalSection(&G_gui.snapshot_lock);
   InitializeCriticalSection(&G_gui.coe_lock);

   icc.dwSize = sizeof(icc);
   icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
   InitCommonControlsEx(&icc);

   hwnd = CreateDialogParamA(instance, MAKEINTRESOURCEA(IDD_ETHERCAT_GUI),
                             NULL, MainDlgProc, 0);
   if (hwnd == NULL)
   {
      MessageBoxA(NULL, "CreateDialogParam failed.", APP_TITLE,
                  MB_ICONERROR | MB_OK);
      DeleteCriticalSection(&G_gui.log_lock);
      DeleteCriticalSection(&G_gui.snapshot_lock);
      DeleteCriticalSection(&G_gui.coe_lock);
      return 1;
   }

   ShowWindow(hwnd, show_cmd);
   UpdateWindow(hwnd);

   while (GetMessageA(&msg, NULL, 0, 0) > 0)
   {
      if (!IsDialogMessageA(hwnd, &msg))
      {
         TranslateMessage(&msg);
         DispatchMessageA(&msg);
      }
   }

   DeleteCriticalSection(&G_gui.log_lock);
   DeleteCriticalSection(&G_gui.snapshot_lock);
   DeleteCriticalSection(&G_gui.coe_lock);
   return (int)msg.wParam;
}
