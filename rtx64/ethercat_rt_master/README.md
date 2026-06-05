# RTX64 EtherCAT RT Master

This folder is the phase-2 scaffold for moving the cyclic EtherCAT PDO loop out
of the normal Windows process and into an RTX64 real-time process.

Current status:

- The normal Windows DLL/GUI build remains the phase-1 diagnostic tool.
- This project is disabled by default.
- The shared-memory command/status contract is defined in `include/ecat_rt_ipc.h`.
- The real RTX64 NAL/RTND Ethernet transport is intentionally left as the next
  hardware-dependent implementation step.

Recommended runtime architecture:

```text
Windows process
  ethercat_gui.exe
  ethercat_dll.dll
  XML database
  command/status UI

RTX64 RTSS process
  ethercat_rt_master
  cyclic PDO loop
  CiA402 state machines
  LMS motion scheduling

IPC
  shared memory
  command queue
  status snapshot
```

Enable this target after installing RTX64 SDK:

```powershell
cmake --preset windows-msvc-vs2022 -DSOEM_BUILD_RTX64_MASTER=ON `
  -DRTX64_SDK_DIR="C:/Program Files/IntervalZero/RTX64 SDK"
cmake --build --preset windows-msvc-vs2022 --target ethercat_rt_master
```

The SDK path and library names can vary by RTX64 version, so the CMake target
uses these cache variables:

```text
RTX64_SDK_DIR
RTX64_INCLUDE_DIR
RTX64_LIBRARY_DIR
RTX64_LIBRARIES
```

Next implementation tasks:

1. Bind RTX64 shared memory for `ECAT_RtSharedMemory`.
2. Replace the scaffold cycle with a real high-priority periodic timer.
3. Implement RTX64 real-time Ethernet send/receive through the supported NIC.
4. Port the SOEM OSAL/OSHW layer to RTX64.
5. Move CiA402 PDO command generation into the RT cycle.
