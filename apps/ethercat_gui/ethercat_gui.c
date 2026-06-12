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
   HWND motion_velocity;
   HWND motion_accel;
   HWND motion_decel;
   HWND motion_home_method;
   HWND motion_profile;
   HWND motion_fault_reset;
   HWND motion_enable;
   HWND motion_disable;
   HWND motion_stop;
   HWND motion_jog_pos;
   HWND motion_jog_neg;
   HWND motion_move_abs;
   HWND motion_move_rel;
   HWND motion_home;
   HWND motion_status;
   HWND stats_edit;
   HWND xml_import;
   HWND xml_reload;
   HWND xml_list;
   HWND xml_detail;
   HWND log_edit;
   HWND sdo_labels[4];
   HWND motion_labels[7];
   HWND rt_labels[2];
   int active_tab;
   int selected_slave;
   CRITICAL_SECTION log_lock;
   char log_text[MAX_GUI_LOG];
   int log_len;
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
   int show_motion = (tab == 3) ? SW_SHOW : SW_HIDE;
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

   for (i = 0; i < 7; ++i)
   {
      ShowWindow(G_gui.motion_labels[i], show_motion);
   }
   ShowWindow(G_gui.motion_profile, show_motion);
   ShowWindow(G_gui.motion_slave, show_motion);
   ShowWindow(G_gui.motion_target, show_motion);
   ShowWindow(G_gui.motion_velocity, show_motion);
   ShowWindow(G_gui.motion_accel, show_motion);
   ShowWindow(G_gui.motion_decel, show_motion);
   ShowWindow(G_gui.motion_home_method, show_motion);
   ShowWindow(G_gui.motion_fault_reset, show_motion);
   ShowWindow(G_gui.motion_enable, show_motion);
   ShowWindow(G_gui.motion_disable, show_motion);
   ShowWindow(G_gui.motion_stop, show_motion);
   ShowWindow(G_gui.motion_jog_pos, show_motion);
   ShowWindow(G_gui.motion_jog_neg, show_motion);
   ShowWindow(G_gui.motion_move_abs, show_motion);
   ShowWindow(G_gui.motion_move_rel, show_motion);
   ShowWindow(G_gui.motion_home, show_motion);
   ShowWindow(G_gui.motion_status, show_motion);

   ShowWindow(G_gui.stats_edit, tab == 4 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_import, tab == 5 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_reload, tab == 5 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_list, tab == 5 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.xml_detail, tab == 5 ? SW_SHOW : SW_HIDE);
   ShowWindow(G_gui.log_edit, tab == 6 ? SW_SHOW : SW_HIDE);
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
   G_gui.motion_velocity = GetDlgItem(hwnd, IDC_MOTION_VELOCITY);
   G_gui.motion_accel = GetDlgItem(hwnd, IDC_MOTION_ACCEL);
   G_gui.motion_decel = GetDlgItem(hwnd, IDC_MOTION_DECEL);
   G_gui.motion_home_method = GetDlgItem(hwnd, IDC_MOTION_HOME_METHOD);
   G_gui.motion_profile = GetDlgItem(hwnd, IDC_MOTION_PROFILE);
   G_gui.motion_fault_reset = GetDlgItem(hwnd, IDC_MOTION_FAULT_RESET);
   G_gui.motion_enable = GetDlgItem(hwnd, IDC_MOTION_ENABLE);
   G_gui.motion_disable = GetDlgItem(hwnd, IDC_MOTION_DISABLE);
   G_gui.motion_stop = GetDlgItem(hwnd, IDC_MOTION_STOP);
   G_gui.motion_jog_pos = GetDlgItem(hwnd, IDC_MOTION_JOG_POS);
   G_gui.motion_jog_neg = GetDlgItem(hwnd, IDC_MOTION_JOG_NEG);
   G_gui.motion_move_abs = GetDlgItem(hwnd, IDC_MOTION_MOVE_ABS);
   G_gui.motion_move_rel = GetDlgItem(hwnd, IDC_MOTION_MOVE_REL);
   G_gui.motion_home = GetDlgItem(hwnd, IDC_MOTION_HOME);
   G_gui.motion_status = GetDlgItem(hwnd, IDC_MOTION_STATUS);
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
   SendMessageA(G_gui.motion_velocity, EM_SETLIMITTEXT, 12, 0);
   SendMessageA(G_gui.motion_accel, EM_SETLIMITTEXT, 10, 0);
   SendMessageA(G_gui.motion_decel, EM_SETLIMITTEXT, 10, 0);
   SendMessageA(G_gui.motion_home_method, EM_SETLIMITTEXT, 6, 0);
   SetWindowTextA(G_gui.period, "1000");
   SetWindowTextA(G_gui.rt_host, "192.168.100.20");
   SetWindowTextA(G_gui.rt_port, "15000");
   SetWindowTextA(G_gui.sdo_slave, "1");
   SetWindowTextA(G_gui.sdo_index, "0x6041");
   SetWindowTextA(G_gui.sdo_sub, "0");
   SetWindowTextA(G_gui.sdo_size, "64");
   SetWindowTextA(G_gui.motion_slave, "1");
   SetWindowTextA(G_gui.motion_target, "100");
   SetWindowTextA(G_gui.motion_velocity, "10");
   SetWindowTextA(G_gui.motion_accel, "0");
   SetWindowTextA(G_gui.motion_decel, "0");
   SetWindowTextA(G_gui.motion_home_method, "35");

   add_tab(G_gui.tabs, "Slave Status", 0);
   add_tab(G_gui.tabs, "PDO Monitor", 1);
   add_tab(G_gui.tabs, "SDO Browser", 2);
   add_tab(G_gui.tabs, "Motion Control", 3);
   add_tab(G_gui.tabs, "Communication", 4);
   add_tab(G_gui.tabs, "XML Database", 5);
   add_tab(G_gui.tabs, "Log", 6);
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

   MoveWindow(G_gui.motion_labels[6], tab_rc.left, tab_y + 4, 82, 22, TRUE);
   MoveWindow(G_gui.motion_profile, tab_rc.left + 86, tab_y, 132, 120, TRUE);
   MoveWindow(G_gui.motion_labels[0], tab_rc.left + 232, tab_y + 4, 40, 22, TRUE);
   MoveWindow(G_gui.motion_slave, tab_rc.left + 276, tab_y, 50, 24, TRUE);
   MoveWindow(G_gui.motion_labels[1], tab_rc.left + 340, tab_y + 4, 44, 22, TRUE);
   MoveWindow(G_gui.motion_target, tab_rc.left + 386, tab_y, 82, 24, TRUE);
   MoveWindow(G_gui.motion_labels[2], tab_rc.left, tab_y + 34, 58, 22, TRUE);
   MoveWindow(G_gui.motion_velocity, tab_rc.left + 62, tab_y + 30, 82, 24, TRUE);
   MoveWindow(G_gui.motion_labels[3], tab_rc.left + 158, tab_y + 34, 44, 22, TRUE);
   MoveWindow(G_gui.motion_accel, tab_rc.left + 204, tab_y + 30, 70, 24, TRUE);
   MoveWindow(G_gui.motion_labels[4], tab_rc.left + 286, tab_y + 34, 44, 22, TRUE);
   MoveWindow(G_gui.motion_decel, tab_rc.left + 332, tab_y + 30, 70, 24, TRUE);
   MoveWindow(G_gui.motion_labels[5], tab_rc.left + 414, tab_y + 34, 44, 22, TRUE);
   MoveWindow(G_gui.motion_home_method, tab_rc.left + 460, tab_y + 30, 50, 24, TRUE);
   MoveWindow(G_gui.motion_fault_reset, tab_rc.left, tab_y + 66, 96, 26, TRUE);
   MoveWindow(G_gui.motion_enable, tab_rc.left + 104, tab_y + 66, 76, 26, TRUE);
   MoveWindow(G_gui.motion_disable, tab_rc.left + 188, tab_y + 66, 76, 26, TRUE);
   MoveWindow(G_gui.motion_stop, tab_rc.left + 272, tab_y + 66, 76, 26, TRUE);
   MoveWindow(G_gui.motion_jog_pos, tab_rc.left, tab_y + 98, 72, 26, TRUE);
   MoveWindow(G_gui.motion_jog_neg, tab_rc.left + 80, tab_y + 98, 72, 26, TRUE);
   MoveWindow(G_gui.motion_move_abs, tab_rc.left + 160, tab_y + 98, 82, 26, TRUE);
   MoveWindow(G_gui.motion_move_rel, tab_rc.left + 250, tab_y + 98, 82, 26, TRUE);
   MoveWindow(G_gui.motion_home, tab_rc.left + 340, tab_y + 98, 60, 26, TRUE);
   MoveWindow(G_gui.motion_status, tab_rc.left, tab_y + 132,
              tab_rc.right - tab_rc.left, tab_rc.bottom - tab_y - 132, TRUE);

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

static void update_list_views(const ECAT_RuntimeStatus *runtime)
{
   int i;

   ListView_DeleteAllItems(G_gui.slaves);
   ListView_DeleteAllItems(G_gui.status_list);

   for (i = 1; i <= runtime->slave_count; ++i)
   {
      ECAT_SlaveInfo s;
      char b0[64], b1[64], b2[64], b3[64], b4[64];

      if (ECAT_GetSlaveInfo(i, &s) != ECAT_OK)
      {
         continue;
      }

      (void)snprintf(b0, sizeof(b0), "%d", s.index);
      insert_row(G_gui.slaves, i - 1, b0);
      set_row_text(G_gui.slaves, i - 1, 1, s.name);
      set_row_text(G_gui.slaves, i - 1, 2, ECAT_StateName(s.state));
      (void)snprintf(b1, sizeof(b1), "%uO/%uI", s.output_bytes,
                     s.input_bytes);
      set_row_text(G_gui.slaves, i - 1, 3, b1);

      insert_row(G_gui.status_list, i - 1, b0);
      set_row_text(G_gui.status_list, i - 1, 1, s.name);
      set_row_text(G_gui.status_list, i - 1, 2, ECAT_StateName(s.state));
      (void)snprintf(b1, sizeof(b1), "0x%04X", s.al_status);
      set_row_text(G_gui.status_list, i - 1, 3, b1);
      (void)snprintf(b2, sizeof(b2), "%u B", s.output_bytes);
      (void)snprintf(b3, sizeof(b3), "%u B", s.input_bytes);
      set_row_text(G_gui.status_list, i - 1, 4, b2);
      set_row_text(G_gui.status_list, i - 1, 5, b3);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s.vendor_id);
      set_row_text(G_gui.status_list, i - 1, 6, b4);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s.product_code);
      set_row_text(G_gui.status_list, i - 1, 7, b4);
      (void)snprintf(b4, sizeof(b4), "0x%08X", s.revision);
      set_row_text(G_gui.status_list, i - 1, 8, b4);
      set_row_text(G_gui.status_list, i - 1, 9, s.has_dc ? "Yes" : "No");
      set_row_text(G_gui.status_list, i - 1, 10,
                   s.database_matched ? s.database_name : "-");

      if (s.index == G_gui.selected_slave)
      {
         ListView_SetItemState(G_gui.slaves, i - 1,
                               LVIS_SELECTED | LVIS_FOCUSED,
                               LVIS_SELECTED | LVIS_FOCUSED);
      }
   }
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

static void format_pdo_text(char *dst, size_t dst_size, int slave_index)
{
   ECAT_SlaveInfo s;
   unsigned char outputs[ECAT_MAX_PDO_COPY];
   unsigned char inputs[ECAT_MAX_PDO_COPY];
   int output_size = 0;
   int input_size = 0;
   char hex[4096];
   size_t used = 0;

   if (ECAT_GetSlaveInfo(slave_index, &s) != ECAT_OK)
   {
      safe_copy(dst, dst_size, "Select a slave module from the left list.\r\n");
      return;
   }

   (void)ECAT_GetPdoSnapshot(slave_index, outputs, sizeof(outputs),
                             &output_size, inputs, sizeof(inputs),
                             &input_size);

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "Slave %d - %s\r\nState: %s (0x%04X), AL: 0x%04X\r\n"
                            "Config address: 0x%04X, Alias: 0x%04X\r\n"
                            "Mailbox: W=%u B, R=%u B, Proto=0x%04X\r\n"
                            "XML DB: %s%s%s\r\n\r\n",
                            s.index, s.name, ECAT_StateName(s.state), s.state,
                            s.al_status, s.config_address, s.alias_address,
                            s.mailbox_write_bytes, s.mailbox_read_bytes,
                            s.mailbox_protocols,
                            s.database_matched ? s.database_name : "Not matched",
                            s.database_matched ? " | " : "",
                            s.database_matched ? s.database_xml : "");

   if (input_size >= 2)
   {
      unsigned short statusword = read_u16_le(inputs);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Input[0..1] as CiA 402 statusword: 0x%04X (%s)\r\n",
                               statusword, ECAT_Cia402StateName(statusword));
   }
   if (output_size >= 2)
   {
      unsigned short controlword = read_u16_le(outputs);
      used += (size_t)snprintf(dst + used, dst_size - used,
                               "Output[0..1] as CiA 402 controlword: 0x%04X\r\n",
                               controlword);
   }

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "\r\nOutputs/RxPDO snapshot (%d of %u bytes copied)\r\n",
                            output_size, s.output_bytes);
   hex_dump(hex, sizeof(hex), outputs, output_size);
   used += (size_t)snprintf(dst + used, dst_size - used, "%s", hex);

   used += (size_t)snprintf(dst + used, dst_size - used,
                            "\r\nInputs/TxPDO snapshot (%d of %u bytes copied)\r\n",
                            input_size, s.input_bytes);
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

static void format_motion_status(char *dst, size_t dst_size)
{
   ECAT_ServoStatus status;
   int slave = get_motion_slave();
   int result;
   int profile_type = ECAT_PROFILE_TRAPEZOIDAL;
   double jerk_ratio = 0.0;

   if (!ECAT_IsOpen())
   {
      safe_copy(dst, dst_size, "Motion backend is disconnected.\r\n");
      return;
   }

   memset(&status, 0, sizeof(status));
   result = ECAT_ServoGetStatus(slave, &status);
   if (result != ECAT_OK)
   {
      (void)snprintf(dst, dst_size,
                     "Servo status read failed.\r\n"
                     "Slave: %d\r\n"
                     "Result: %s (%d)\r\n",
                     slave, ECAT_ErrorToString(result), result);
      return;
   }

   (void)ECAT_GetMotionProfile(&profile_type, &jerk_ratio);
   (void)snprintf(dst, dst_size,
                  "Selected slave\r\n"
                  "  Slave              : %d\r\n\r\n"
                  "CiA402 Status\r\n"
                  "  State              : %s\r\n"
                  "  Statusword         : 0x%04X\r\n"
                  "  Controlword        : 0x%04X\r\n"
                  "  Mode display       : %d\r\n"
                  "  Actual position    : %d\r\n"
                  "  Actual velocity    : %d\r\n"
                  "  Error code         : 0x%04X\r\n"
                  "  Target reached     : %s\r\n"
                  "  Fault              : %s\r\n"
                  "  Warning            : %s\r\n\r\n"
                  "Command fields\r\n"
                  "  Target position    : %d\r\n"
                  "  Velocity/Jog       : %d\r\n"
                  "  Acceleration       : %u\r\n"
                  "  Deceleration       : %u\r\n"
                  "  Homing method      : %d\r\n"
                  "  Profile type       : %s\r\n"
                  "  Jerk ratio         : %.2f\r\n",
                  slave,
                  status.cia402_state,
                  status.statusword,
                  status.controlword,
                  status.mode_display,
                  status.actual_position,
                  status.actual_velocity,
                  status.error_code,
                  status.target_reached ? "Yes" : "No",
                  status.fault ? "Yes" : "No",
                  status.warning ? "Yes" : "No",
                  get_edit_i32(G_gui.motion_target, 0),
                  get_edit_i32(G_gui.motion_velocity, 0),
                  get_edit_u32(G_gui.motion_accel, 0),
                  get_edit_u32(G_gui.motion_decel, 0),
                  get_edit_i32(G_gui.motion_home_method, 35),
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

static void request_motion_command(int control_id)
{
   int slave = get_motion_slave();
   int target = get_edit_i32(G_gui.motion_target, 0);
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

static void update_gui(void)
{
   ECAT_RuntimeStatus runtime;
   char summary[512];
   char text[32768];
   char logs[MAX_GUI_LOG];

   memset(&runtime, 0, sizeof(runtime));
   ECAT_GetRuntimeStatus(&runtime);

   (void)snprintf(summary, sizeof(summary),
                  "%s | Slaves: %d | WKC: %d/%d | Cycle: %d us "
                  "(avg %.1f us, min %d, max %d) | WKC errors: %d",
                  runtime.state_text, runtime.slave_count, runtime.last_wkc,
                  runtime.expected_wkc, runtime.cycle_us, runtime.avg_cycle_us,
                  runtime.min_cycle_us, runtime.max_cycle_us,
                  runtime.wkc_errors);
   SetWindowTextA(G_gui.summary, summary);

   update_list_views(&runtime);

   format_pdo_text(text, sizeof(text), G_gui.selected_slave);
   SetWindowTextA(G_gui.pdo_edit, text);

   format_stats_text(text, sizeof(text), &runtime);
   SetWindowTextA(G_gui.stats_edit, text);

   if (G_gui.active_tab == 3)
   {
      format_motion_status(text, sizeof(text));
      SetWindowTextA(G_gui.motion_status, text);
   }

   EnterCriticalSection(&G_gui.log_lock);
   safe_copy(logs, sizeof(logs), G_gui.log_text);
   LeaveCriticalSection(&G_gui.log_lock);
   SetWindowTextA(G_gui.log_edit, logs);
   SendMessageA(G_gui.log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
   SendMessageA(G_gui.log_edit, EM_SCROLLCARET, 0, 0);

   EnableWindow(G_gui.connect, !ECAT_IsOpen());
   EnableWindow(G_gui.disconnect, ECAT_IsOpen());
   EnableWindow(G_gui.sdo_read, ECAT_IsOpen());
   EnableWindow(G_gui.motion_fault_reset, ECAT_IsOpen());
   EnableWindow(G_gui.motion_enable, ECAT_IsOpen());
   EnableWindow(G_gui.motion_disable, ECAT_IsOpen());
   EnableWindow(G_gui.motion_stop, ECAT_IsOpen());
   EnableWindow(G_gui.motion_jog_pos, ECAT_IsOpen());
   EnableWindow(G_gui.motion_jog_neg, ECAT_IsOpen());
   EnableWindow(G_gui.motion_move_abs, ECAT_IsOpen());
   EnableWindow(G_gui.motion_move_rel, ECAT_IsOpen());
   EnableWindow(G_gui.motion_home, ECAT_IsOpen());
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
      SetTimer(hwnd, UPDATE_TIMER_ID, UPDATE_TIMER_MS, NULL);
      append_log_line("INFO", "SOEM EtherCAT Master Monitor started");
      return TRUE;

   case WM_SIZE:
      layout_controls(hwnd);
      return TRUE;

   case WM_TIMER:
      if (wparam == UPDATE_TIMER_ID)
      {
         update_gui();
      }
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
      case IDC_MOTION_MOVE_REL:
      case IDC_MOTION_HOME:
         request_motion_command(LOWORD(wparam));
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
      ECAT_Close();
      DestroyWindow(hwnd);
      return TRUE;

   case WM_DESTROY:
      KillTimer(hwnd, UPDATE_TIMER_ID);
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
   return (int)msg.wParam;
}
