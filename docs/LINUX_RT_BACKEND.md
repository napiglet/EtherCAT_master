# Linux RT Backend

This backend keeps the Windows GUI and SDK API intact while moving the real-time
EtherCAT loop to a Linux RT controller.

## Architecture

```text
Windows PC
  ethercat_gui.exe
  ethercat_dll.dll
  XML database
  Windows Debug Backend
  Linux RT Backend TCP client

Linux RT Controller
  ethercat_rt_core
  TCP server
  mock status for Windows integration tests
  IgH/Xenomai cyclic PDO monitor for real slave bring-up
  IgH/Xenomai TCP backend server for Windows GUI/DLL integration
```

## Current Status

- `common/ecat_protocol.h` defines the shared TCP packet format.
- `linux_rt/ethercat_rt_core` builds a mock RT server.
- `ethercat_igh_lts_monitor` is a Linux-only IgH/Xenomai cyclic PDO monitor
  for the detected `LTS_MotorDriver1x` slave.
- `ethercat_igh_backend_server` is a Linux-only IgH/Xenomai TCP backend server
  that runs the real cyclic PDO loop and talks to the Windows GUI/DLL over the
  existing protocol.
- The backend now contains the first generic CiA402 motion profile layer:
  - `MoveAbs` / `MoveRel`: internal trapezoidal position profile, emitted as
    cyclic synchronous position PDO targets.
  - `Jog` / `MoveVel`: cyclic synchronous velocity command with acceleration
    and deceleration ramping.
  - `Stop`: controlled velocity ramp to zero with CiA402 halt controlword.
  - `Home`: homing mode command structure with velocity ramp support.
- `ethercat_dll` can select either backend:
  - `ECAT_BACKEND_WINDOWS_DEBUG`
  - `ECAT_BACKEND_LINUX_RT`
- `ethercat_gui` has a backend selector, host, and port fields.

## SDK API

```c
ECAT_SetBackend(ECAT_BACKEND_LINUX_RT);
ECAT_SetLinuxRtEndpoint("192.168.0.10", 15000);
ECAT_Open("Linux RT Controller", &options);

ECAT_GetRuntimeStatus(&status);
ECAT_GetSlaveInfo(1, &slave);
ECAT_ServoEnable(1);
ECAT_ServoMoveAbs(1, 12345, 5000, 10000, 10000);
ECAT_ServoMoveRel(1, -1000, 5000, 10000, 10000);
ECAT_ServoMoveVel(1, 100, 1000, 1000);
ECAT_ServoStop(1);
ECAT_LMS_MoveAbs(1, 12345, 5000);

ECAT_Close();
```

## Local Mock Test On Windows

Build the mock server target:

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --preset windows-msvc-vs2022 `
  -DSOEM_BUILD_LINUX_RT_CORE=ON
& "C:\Program Files\CMake\bin\cmake.exe" --build --preset windows-msvc-vs2022 `
  --target ethercat_rt_core
```

Run the mock server:

```powershell
.\build\windows-msvc-vs2022\bin\RelWithDebInfo\ethercat_rt_core.exe 15000
```

Run the DLL smoke test:

```powershell
.\build\windows-msvc-vs2022\bin\RelWithDebInfo\ethercat_dll_test.exe `
  --linux-rt 127.0.0.1 15000
```

## Linux Mini PC Mock Build

On the Linux RT controller:

```bash
cmake -S . -B build/linux-rt \
  -DSOEM_BUILD_LINUX_RT_CORE=ON \
  -DSOEM_BUILD_ETHERCAT_DLL=OFF \
  -DSOEM_BUILD_ETHERCAT_GUI=OFF
cmake --build build/linux-rt --target ethercat_rt_core
./build/linux-rt/bin/ethercat_rt_core 15000
```

## Xenomai + IgH LTS PDO Monitor

Prerequisites already verified on the mini PC:

```bash
uname -a
cat /proc/xenomai/version
/usr/xenomai/bin/xeno-config --version
sudo ethercat master
sudo ethercat slaves
```

The expected slave identity for the first monitor target:

```text
Name: LTS_MotorDriver1x
Vendor ID: 0x0000205e
Product Code: 0x90000300
RxPDO: 0x6040, 0x607a, 0x60ff, 0x6060
TxPDO: 0x6041, 0x6064, 0x606c, 0x6061
```

Build:

```bash
cmake -S . -B build/linux-xenomai \
  -DSOEM_BUILD_SAMPLES=OFF \
  -DSOEM_BUILD_ETHERCAT_DLL=OFF \
  -DSOEM_BUILD_ETHERCAT_GUI=OFF \
  -DSOEM_BUILD_LMS_MASTER=OFF \
  -DSOEM_BUILD_LINUX_RT_CORE=ON \
  -DECAT_RT_CORE_ENABLE_IGH=ON \
  -DECAT_XENOMAI_CONFIG=/usr/xenomai/bin/xeno-config

cmake --build build/linux-xenomai --target ethercat_igh_lts_monitor -j$(nproc)
cmake --build build/linux-xenomai --target ethercat_igh_backend_server -j$(nproc)
cmake --build build/linux-xenomai --target motion_cia402_test -j$(nproc)
./build/linux-xenomai/bin/motion_cia402_test
```

Safe PDO exchange test. This does not enable the servo; the command outputs are
zero unless explicitly overridden:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 10000 \
  --print-every 1000
```

If the slave is online and OP but the domain stays at `wkc=0/ZERO`, compare
against the slave's default SII PDO map:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 10000 \
  --print-every 1000 \
  --use-sii-pdos
```

Expected milestone:

```text
slave{online=1 op=1 al=0x8}
wkc=.../COMPLETE
err{wkc=0 state=0}
```

The monitor also prints CiA402 state text, mode text, WKC/state error counters,
and cycle timing statistics:

```text
sw=0x1221/ReadyToSwitchOn mode=8/CSP
rt{min/avg/max=.../.../...us jitter_max=...us}
```

## CiA402 State Sequence Tests

The default monitor remains safe and writes `controlword=0x0000`. CiA402
commands are sent only when one of the explicit sequence options is used.
The reusable state parser and Controlword generator live in:

```text
linux_rt/ethercat_rt_core/include/motion_cia402.h
linux_rt/ethercat_rt_core/src/motion_cia402.c
```

Recommended order while the motor is not connected:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 20000 \
  --print-every 1000 \
  --fault-reset

sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 20000 \
  --print-every 1000 \
  --cia402-shutdown

sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 20000 \
  --print-every 1000 \
  --cia402-switch-on
```

`--cia402-enable` is intentionally separate because a drive may refuse
`OperationEnabled` while the motor/power stage/interlocks are not connected:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 20000 \
  --print-every 1000 \
  --cia402-enable \
  --sequence-timeout-ms 5000
```

Expected sequence log fields:

```text
cw=0x0006 seq{Shutdown done=1 fail=0 done_cyc=...}
cw=0x0007 seq{SwitchOn done=1 fail=0 done_cyc=...}
cw=0x000f seq{EnableOperation done=0 fail=1 done_cyc=-1}
```

## Motion Command Structure Tests

The motion command structure is available through `motion_cia402` and explicit
monitor options. Jog/Profile/Home automatically run `EnableOperation` first if
no sequence option is supplied.

Safe stop/halt output:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --servo-stop
```

Jog velocity test. Use a very small velocity until the real motor/stator setup
is ready:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --jog-velocity 10
```

Profile position command structure test:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --profile-position 100 \
  --profile-velocity 10
```

Homing command structure test:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --home \
  --home-velocity 10
```

Expected motion log fields:

```text
seq{EnableOperation done=1 fail=0}
out{tp=... tv=... mode=... motion=JogVelocity}
err{wkc=0 state=0}
```

## IgH/Xenomai TCP Backend Server

The first real Windows GUI/DLL integration target is:

```text
ethercat_igh_backend_server
```

It keeps the IgH cyclic PDO loop running in a realtime thread and serves the
existing Windows protocol on TCP port `15000`. The first backend target is still
LTS-specific, using the PDO map already confirmed on the mini PC.

Build:

```bash
cmake --build build/linux-xenomai --target ethercat_igh_backend_server -j$(nproc)
```

Run:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_backend_server \
  --port 15000 \
  --period-us 1000 \
  --client-timeout-ms 1000
```

If the LTS drive needs the SII/default PDO map:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_backend_server \
  --port 15000 \
  --period-us 1000 \
  --use-sii-pdos \
  --client-timeout-ms 1000
```

Windows side:

1. Select GUI backend `Linux RT`.
2. Set host to the Linux controller IP address.
3. Set port to `15000`.
4. Open the connection.
5. Use the Motion Control tab for Fault Reset, Enable, Stop, Jog, Move Abs,
   Move Rel, and Home commands. The `Target` field is absolute position for
   `Move Abs` and relative distance for `Move Rel`.

The first server supports runtime status, WKC/cycle diagnostics, PDO snapshots,
cached SDO reads/writes for the mapped CiA402 objects, Servo Enable/Disable,
Fault Reset, Jog/MoveVel, trapezoidal MoveAbs/MoveRel, Home, Stop, and LMS
wrapper commands. It also applies a safe stop when the Windows client heartbeat
is absent longer than `--client-timeout-ms`. GUI status polling sends
`ECAT_NET_CMD_NONE`, which serves as the heartbeat.

## Next Implementation Step

1. Test `ethercat_igh_backend_server` with the Windows GUI over the network.
2. Tune motion limits with the real servo/motor connected:
   velocity, acceleration, deceleration, following error, and soft limits.
3. Move the LTS-specific PDO profile into the XML database/profile loader.
4. Add multi-slave support and per-slave CiA402 state machines.
5. Start LMS-specific mover/stator abstractions above the generic motion layer.
