# Linux RT EtherCAT Core

This is the first Linux RT backend target for the Windows GUI/DLL architecture.

Current phase:

- TCP protocol server.
- Mock EtherCAT runtime status.
- Mock single CiA402 slave.
- Accepts motion commands from the Windows DLL.
- IgH/Xenomai LTS_MotorDriver1x PDO monitor target.

Future phase:

1. Keep Windows GUI/DLL protocol stable.
2. Replace the mock cycle with the IgH cyclic PDO backend.
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
