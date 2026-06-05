# EtherCAT Motion Development Roadmap

## Phase 1 - Windows Diagnostic Master

Purpose:

- Keep the current Windows build as the commissioning and debugging tool.
- Provide adapter scan, slave scan, PDO/SDO monitor, cycle statistics, logs, and
  XML database management.
- Keep the distributable SDK shape: `ethercat_dll.dll`,
  `ethercat_dll.lib`, and `ethercat_master.h`.

Current project pieces:

```text
ethercat_dll/
apps/ethercat_gui/
apps/lms_master/
```

## Phase 2 - Linux Xenomai/IgH Real-Time PDO Loop

Purpose:

- Move only the cyclic PDO loop and motion state machines into the Linux RT
  controller.
- Keep Windows GUI/API for setup, XML database, command entry, and diagnostics.

Current controller direction:

```text
Linux mini PC now:
  Xenomai 3 Cobalt
  IgH EtherCAT Master 1.6.9
  LTS_MotorDriver1x detected and OP confirmed

Final target later:
  TI TMDS64EVM / AM64x Linux controller
```

Added project pieces:

```text
linux_rt/ethercat_rt_core/
  src/ethercat_rt_core.c      mock TCP backend
  src/igh_lts_monitor.c       IgH/Xenomai PDO monitor
```

The Linux IgH monitor is disabled by default:

```text
ECAT_RT_CORE_ENABLE_IGH=OFF
```

Enable it on the Xenomai mini PC:

```bash
cmake -S . -B build/linux-xenomai \
  -DSOEM_BUILD_LINUX_RT_CORE=ON \
  -DECAT_RT_CORE_ENABLE_IGH=ON \
  -DECAT_XENOMAI_CONFIG=/usr/xenomai/bin/xeno-config
```

Next phase-2 implementation work:

1. Validate `ethercat_igh_lts_monitor` WKC and OP stability.
2. Convert the monitor into a persistent RT backend loop.
3. Add TCP command/status double buffering.
4. Publish cyclic PDO snapshots to Windows GUI/DLL.
5. Keep RTX64 scaffold as a future optional branch, not the main path.

## Phase 3 - General Servo / CiA402 Motion

Purpose:

- Support normal EtherCAT servo drives before LMS-specific behavior.
- Implement CiA402 profile position, profile velocity jog, homing, stop,
  enable/disable, and fault reset.
- Validate reliability with actual EtherCAT slave motor drivers.

Initial SDK APIs:

```c
ECAT_WriteSdo(...)
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

Current implementation:

- Uses standard CiA402 SDO objects.
- Suitable for early bench tests and drive bring-up.
- Later implementation can route the same APIs through the Linux Xenomai/IgH
  PDO loop.

Important standard objects:

```text
0x6040 Controlword
0x6041 Statusword
0x6060 Modes of operation
0x6061 Modes of operation display
0x607A Target position
0x6081 Profile velocity
0x6083 Profile acceleration
0x6084 Profile deceleration
0x6098 Homing method
0x6099 Homing speeds
0x609A Homing acceleration
0x60FF Target velocity
0x6064 Position actual value
0x606C Velocity actual value
```

## Phase 4 - LMS Motion API

Purpose:

- Add WMX-style LMS APIs on top of the proven servo/RT layer.
- Hide internal LMS/stator/mover control details behind the SDK.

Initial SDK APIs:

```c
ECAT_LMS_MoveAbs(...)
ECAT_LMS_MoveVel(...)
ECAT_LMS_Stop(...)
ECAT_LMS_GetMoverPosition(...)
```

Current implementation:

- Thin wrappers over the general CiA402 servo API.
- The API names are stable, but the internals will evolve after real LMS slave
  driver testing.

Future LMS-specific work:

1. Model stator segments and mover identity.
2. Add mover-to-stator handoff logic.
3. Add coil/stator current limits and safety interlocks.
4. Add multi-mover collision prevention.
5. Add LMS-specific diagnostics and tuning views in the GUI.
