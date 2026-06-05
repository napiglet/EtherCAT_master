# Simple Open EtherCAT Master Library

* Copyright (C) 2005-2025 Speciaal Machinefabriek Ketels v.o.f.
* Copyright (C) 2005-2025 Arthur Ketels
* Copyright (C) 2009-2025 RT-Labs AB, Sweden

SOEM (Simple Open EtherCAT Master) is a software library for
developing EtherCAT MainDevices.

This library is specifically designed for real-time communication in
embedded systems. Its lightweight architecture minimizes resource
consumption, making it suitable for environments with limited
resources. SOEM can also be utilized on both Linux and Windows
systems.

As a library rather than a standalone application, SOEM provides
flexibility and customization for developers looking to implement
EtherCAT technology. 

# Documentation

See https://docs.rt-labs.com/soem

# LMS EtherCAT Master application

This workspace also contains a Windows/VS Code starter application for
software-based LMS motion control:

* `apps/lms_master` - EtherCAT scan, cyclic monitor, LMS stator map validation,
  and optional CiA 402 PDO output prototype.
* `apps/ethercat_gui` - native Windows GUI monitor for slave modules, slave
  status, PDO snapshots, SDO reads, communication cycle timing, and WKC/CRC
  diagnostics.
* `ethercat_dll` - distributable EtherCAT API wrapper that builds
  `ethercat_dll.dll`, `ethercat_dll.lib`, and `ethercat_master.h`.
* `rtx64/ethercat_rt_master` - RTX64 real-time master scaffold for the future
  cyclic PDO loop.
* `linux_rt/ethercat_rt_core` - Linux RT backend mock server that will become
  the PREEMPT_RT EtherCAT cyclic controller.
* `config/lms_bosch_sample.csv` - sample Stator Driver + Coil segment map.
* `docs/WINDOWS_VSCODE_SETUP.md` - Windows toolchain and commissioning guide.
* `docs/PORTABLE_BUILD.md` - how to rebuild after copying or renaming the
  project folder without carrying stale absolute Visual Studio paths.
* `docs/DLL_SDK_API.md` - public DLL/LIB/header distribution model and GUI
  sample notes.
* `docs/LINUX_RT_BACKEND.md` - Windows GUI/DLL to Linux RT Core backend guide.
* `docs/PHASED_MOTION_ROADMAP.md` - phase 1-4 plan for Windows diagnostics,
  RTX64 real-time loop, CiA402 servo motion, and LMS motion API.
* `docs/WMX3_BENCHMARK_AND_LMS_ROADMAP.md` - WMX3 benchmark notes and staged
  LMS development roadmap.

# Contributions

Contributions are welcome. If you want to contribute you will need to
sign a Contributor License Agreement and send it to us either by
e-mail or by physical mail. More information is available on
[https://rt-labs.com/contribution](https://rt-labs.com/contribution).
