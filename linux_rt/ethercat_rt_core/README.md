# Linux RT EtherCAT Core

This is the first Linux RT backend target for the Windows GUI/DLL architecture.

Current phase:

- TCP protocol server.
- Mock EtherCAT runtime status.
- Mock single CiA402 slave.
- Accepts motion commands from the Windows DLL.
- IgH/Xenomai LTS_MotorDriver1x PDO monitor target.
- IgH/Xenomai TCP backend server target for Windows GUI/DLL integration.

Future phase:

1. Keep Windows GUI/DLL protocol stable.
2. Move the first LTS-specific IgH backend to a generic XML/profile-driven
   backend.
3. Keep the TCP protocol stable so the Windows GUI and DLL do not change.

Run on Linux:

```bash
cmake -S . -B build/linux-rt -DSOEM_BUILD_LINUX_RT_CORE=ON \
  -DSOEM_BUILD_ETHERCAT_DLL=OFF \
  -DSOEM_BUILD_ETHERCAT_GUI=OFF
cmake --build build/linux-rt --target ethercat_rt_core
./build/linux-rt/bin/ethercat_rt_core 15000
```

The default TCP port is `15000`.

## IgH/Xenomai LTS monitor

This target is Linux-only and is disabled by default. It uses the IgH
application interface and the PDO map confirmed from `LTS_MotorDriver1x`:

- RxPDO `0x1600`: `0x6040`, `0x607a`, `0x60ff`, `0x6060`
- TxPDO `0x1a00`: `0x6041`, `0x6064`, `0x606c`, `0x6061`
- Vendor ID: `0x0000205e`
- Product Code: `0x90000300`

The shared CiA402 state parser and Controlword sequence generator are in
`include/motion_cia402.h` and `src/motion_cia402.c`.

Build on the Xenomai mini PC:

```bash
cd ~/EtherCAT_master
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
```

Safe first run. This exchanges PDOs but leaves the drive command outputs at
zero:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 10000 \
  --print-every 1000
```

If the slave is online and OP but the domain stays at `wkc=0/ZERO`, compare
with the slave/default SII PDO map:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 10000 \
  --print-every 1000 \
  --use-sii-pdos
```

The expected first milestone is stable output like:

```text
wkc=.../COMPLETE master{slaves=1 ... link=1} slave{online=1 op=1 al=0x8} err{wkc=0 state=0}
sw=0x1221/ReadyToSwitchOn mode=8/CSP
rt{min/avg/max=.../.../...us jitter_max=...us}
```

CiA402 sequence options are explicit, so the default monitor stays passive:

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

Use `--cia402-enable` only as an explicit bench test. A drive can reject
`OperationEnabled` while the motor, power stage, or interlocks are not ready.

Motion command structure tests:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --servo-stop

sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --jog-velocity 10

sudo ./build/linux-xenomai/bin/ethercat_igh_lts_monitor \
  --period-us 1000 \
  --cycles 5000 \
  --print-every 1000 \
  --profile-position 100 \
  --profile-velocity 10
```

## IgH/Xenomai TCP backend server

This is the first real Windows GUI/DLL integration target. It runs the IgH
cyclic PDO loop continuously and exposes the existing `common/ecat_protocol.h`
TCP protocol on port `15000`.

Run on the Linux RT controller:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_backend_server \
  --port 15000 \
  --period-us 1000 \
  --client-timeout-ms 1000
```

If the PDO map must use the slave's default SII map:

```bash
sudo ./build/linux-xenomai/bin/ethercat_igh_backend_server \
  --port 15000 \
  --period-us 1000 \
  --use-sii-pdos \
  --client-timeout-ms 1000
```

Then on Windows select the GUI backend as `Linux RT`, set the Linux controller
IP address and port `15000`, and open the connection. The GUI/DLL can now read
runtime status, WKC/cycle statistics, PDO snapshots, cached SDO values, and send
CiA402 commands through the same API used by the mock backend.

The backend applies a safe stop when no Windows client message arrives before
`--client-timeout-ms`. Normal GUI status polling sends `ECAT_NET_CMD_NONE`, so
it also acts as the heartbeat.
