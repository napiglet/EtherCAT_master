# EtherCAT DLL SDK API

This project is structured so the final user-facing package can be shipped as
`DLL + LIB + H`, similar to a WMX-style SDK. Application code should include
only `ethercat_master.h` and link against `ethercat_dll.lib`.

## Project Layout

```text
ethercat_dll/
  include/ethercat_master.h
  src/ethercat_master.c
  test/ethercat_dll_test.c

apps/ethercat_gui/
  ethercat_gui.c
  ethercat_gui.rc
  resource.h
```

Role of each part:

- `ethercat_dll`: wraps SOEM context, EtherCAT worker thread, PDO/SDO access,
  and XML slave database management.
- `ethercat_master.h`: public API header for external applications.
- `ethercat_dll_test`: SDK smoke test and XML import test.
- `ethercat_gui`: sample monitor application that consumes only the public DLL
  API.

## Build Output

The SDK folder is generated automatically after build:

```text
build/windows-msvc-vs2022/ethercat_sdk/
  include/ethercat_master.h
  lib/ethercat_dll.lib
  bin/ethercat_dll.dll
  bin/ethercat_dll_test.exe
  examples/ethercat_dll_test.c
```

The GUI runtime folder also receives the DLL automatically:

```text
build/windows-msvc-vs2022/bin/
  ethercat_gui.exe
  ethercat_dll.dll
```

## Core API

```c
ECAT_ListAdapters(...)
ECAT_ErrorToString(...)
ECAT_GetLastError(...)
ECAT_SetBackend(...)
ECAT_GetBackend(...)
ECAT_SetLinuxRtEndpoint(...)
ECAT_GetLinuxRtEndpoint(...)
ECAT_Open(...)
ECAT_Close()
ECAT_GetRuntimeStatus(...)
ECAT_GetSlaveInfo(...)
ECAT_GetPdoSnapshot(...)
ECAT_ReadSdo(...)
ECAT_WriteSdo(...)
ECAT_StateName(...)
ECAT_Cia402StateName(...)
```

## Backend Selection

The DLL keeps the public API stable while allowing two internal backends:

```c
ECAT_BACKEND_WINDOWS_DEBUG
ECAT_BACKEND_LINUX_RT
```

`ECAT_BACKEND_WINDOWS_DEBUG` is the existing local Windows/SOEM/Npcap backend.
`ECAT_BACKEND_LINUX_RT` connects the same API to a Linux RT controller over TCP.

```c
ECAT_SetBackend(ECAT_BACKEND_LINUX_RT);
ECAT_SetLinuxRtEndpoint("192.168.0.10", 15000);
ECAT_Open("Linux RT Controller", &opt);
```

## XML Database API

The DLL creates a runtime database folder next to the executable:

```text
ethercat_db/
  slaves.csv
  xml/
```

Imported ESI/ENI XML files are copied into `ethercat_db/xml`, and parsed device
identity rows are stored in `slaves.csv`.

```c
ECAT_DbSetRoot(...)
ECAT_DbGetRoot(...)
ECAT_DbReload()
ECAT_DbImportXml(...)
ECAT_DbGetCount(...)
ECAT_DbGetEntry(...)
ECAT_DbFindDevice(...)
```

When the EtherCAT bus is scanned, `ECAT_GetSlaveInfo()` automatically reports
database match data in:

```c
ECAT_SlaveInfo.database_matched
ECAT_SlaveInfo.database_name
ECAT_SlaveInfo.database_xml
```

Matching is performed by Vendor ID + Product Code + Revision. If an exact
revision is not found, the DLL falls back to another entry with the same Vendor
ID and Product Code.

## Servo / CiA402 Motion API

The first motion layer is a standard CiA402 SDO-based API. It is useful for
drive bring-up and bench tests before the RTX64 cyclic PDO implementation is
complete.

```c
ECAT_ServoGetStatus(...)
ECAT_ServoFaultReset(...)
ECAT_ServoEnable(...)
ECAT_ServoDisable(...)
ECAT_ServoSetMode(...)
ECAT_ServoMoveAbs(...)
ECAT_ServoJog(...)
ECAT_ServoHome(...)
ECAT_ServoStop(...)
```

Supported mode constants:

```c
ECAT_CIA402_MODE_PROFILE_POSITION
ECAT_CIA402_MODE_PROFILE_VELOCITY
ECAT_CIA402_MODE_HOMING
ECAT_CIA402_MODE_CSP
ECAT_CIA402_MODE_CSV
ECAT_CIA402_MODE_CST
```

## LMS Motion API

The LMS API is currently a stable public wrapper layer over the general CiA402
servo functions. After LMS slave driver testing, the internals can move to a
dedicated LMS/stator/mover model without changing user application code.

```c
ECAT_LMS_MoveAbs(...)
ECAT_LMS_MoveVel(...)
ECAT_LMS_Stop(...)
ECAT_LMS_GetMoverPosition(...)
```

## Minimal Usage

```c
#include "ethercat_master.h"

ECAT_AdapterInfo adapters[16];
int count = 0;

ECAT_ListAdapters(adapters, 16, &count);

ECAT_OpenOptions opt = {0};
opt.request_operational = 1;
opt.period_us = 1000;

ECAT_Open(adapters[0].name, &opt);

ECAT_RuntimeStatus status;
ECAT_GetRuntimeStatus(&status);

ECAT_Close();
```

## XML Import Test

```powershell
.\build\windows-msvc-vs2022\bin\RelWithDebInfo\ethercat_dll_test.exe `
  .\samples\eni_test\sample-eni.xml
```

## VS Resource View

The static GUI frame is managed by:

```text
apps/ethercat_gui/ethercat_gui.rc
apps/ethercat_gui/resource.h
```

Open `IDD_ETHERCAT_GUI` in VS2022 Resource View to edit basic buttons, list
views, tab controls, and dialog layout. Runtime resizing and live EtherCAT data
updates are still handled in `ethercat_gui.c`.
