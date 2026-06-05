# Windows / VS Code EtherCAT Master Build Guide

이 프로젝트는 SOEM(Simple Open EtherCAT Master)을 기반으로 Windows PC에서
실행되는 LMS 모션 제어용 EtherCAT Master를 개발하기 위한 출발점입니다.

## 1. 필수 설치

1. VS Code
2. VS Code 확장:
   - C/C++ (`ms-vscode.cpptools`)
   - CMake Tools (`ms-vscode.cmake-tools`)
   - CMake language support (`twxs.cmake`)
3. Visual Studio 2022 Build Tools
   - Workload: `Desktop development with C++`
   - Component: MSVC x64/x86, Windows SDK
4. CMake 3.28 이상
5. Npcap
   - EtherCAT frame 송수신을 위해 필요합니다.
   - 설치 시 `WinPcap API-compatible Mode`가 켜져 있어야 합니다.
   - `Admin-only`로 설치한 경우 VS Code 또는 터미널을 관리자 권한으로 실행합니다.
6. EtherCAT 전용 유선 NIC
   - 실험 장비망은 사내/인터넷망과 분리하십시오.
   - Intel NIC 계열을 우선 권장합니다.

현재 이 작업 PC의 PATH에서는 `cmake`와 `ninja`가 발견되지 않았고, 오래된
`cl`/`gcc`만 보입니다. 먼저 최신 VS2022 Build Tools와 CMake를 설치한 뒤
새 터미널에서 확인하십시오.

```powershell
cmake --version
cl
```

## 2. VS Code 빌드

VS Code에서 이 폴더를 열면 `.vscode/settings.json`이
`windows-msvc-vs2022` CMake preset을 기본으로 사용합니다.

터미널에서 직접 빌드할 수도 있습니다.

```powershell
cmake --preset windows-msvc-vs2022
cmake --build --preset windows-msvc-vs2022
```

폴더를 복사했거나 이름을 바꾼 뒤에는 생성된 `build` 폴더 안의 `.sln/.vcxproj`
파일을 그대로 재사용하지 말고 새 위치에서 다시 생성하십시오.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_vs2022.ps1 -Clean
```

자세한 내용은 `docs/PORTABLE_BUILD.md`를 참고하십시오.

빌드 결과:

```text
build\windows-msvc-vs2022\bin\lms_master.exe
build\windows-msvc-vs2022\bin\ethercat_gui.exe
build\windows-msvc-vs2022\bin\ethercat_dll.dll
build\windows-msvc-vs2022\ethercat_sdk\include\ethercat_master.h
build\windows-msvc-vs2022\ethercat_sdk\lib\ethercat_dll.lib
build\windows-msvc-vs2022\ethercat_sdk\bin\ethercat_dll.dll
```

## 3. 첫 실행 순서

1. LMS 설정 파일 검증

```powershell
.\build\windows-msvc-vs2022\bin\lms_master.exe --lms-config .\config\lms_bosch_sample.csv --validate-config
```

2. EtherCAT NIC 이름 확인

```powershell
.\build\windows-msvc-vs2022\bin\lms_master.exe --list-adapters
```

Windows에서는 보통 `\Device\NPF_{GUID}` 형태의 이름을 사용합니다.

GUI로도 확인할 수 있습니다.

```powershell
.\build\windows-msvc-vs2022\bin\ethercat_gui.exe
```

GUI 화면 구성:

- Adapter 선택, Refresh, Connect/Disconnect, OP 요청, cycle period 설정
- Slave Module 리스트
- Slave Status 탭: state, AL status, I/O byte, vendor/product/revision, DC 지원
- PDO Monitor 탭: 선택 slave의 RxPDO/TxPDO hex dump와 CiA 402 status/control word 힌트
- SDO Browser 탭: index/subindex 기반 SDO read
- Communication 탭: requested period, roundtrip min/max/avg, WKC, DC time, CRC/FCS 상태
- Log 탭: 연결, 스캔, SDO, 오류 이벤트

주의: 일반 Npcap/WinPcap 경로에서는 Ethernet FCS/CRC 카운터가 application에
노출되지 않습니다. GUI의 CRC/FCS 항목은 이 제한을 명시하고, 실제 통신 품질은
WKC 오류와 slave state 변화, NIC/switch 진단 카운터를 함께 보도록 구성했습니다.

3. Slave 스캔

```powershell
.\build\windows-msvc-vs2022\bin\lms_master.exe --iface "\Device\NPF_{GUID}" --scan
```

4. 주기 통신 모니터링

```powershell
.\build\windows-msvc-vs2022\bin\lms_master.exe --iface "\Device\NPF_{GUID}" --run --period-us 1000 --cycles 1000
```

5. LMS 가상 mover 경로 검증

```powershell
.\build\windows-msvc-vs2022\bin\lms_master.exe --iface "\Device\NPF_{GUID}" --run --lms-config .\config\lms_bosch_sample.csv --demo-target-mm 250 --period-us 1000 --cycles 5000
```

기본값은 motion PDO를 쓰지 않습니다. 실제 드라이브 PDO offset을 확인하고
`config/lms_bosch_sample.csv`를 수정한 뒤에만 다음 옵션을 사용하십시오.

```powershell
--enable-drive-outputs
```

## 4. LMS Slave Driver 설정 방법

`config/lms_bosch_sample.csv`의 각 행은 하나의 Stator Driver + Coil Set입니다.

```text
slave,name,start_mm,end_mm,coil_pitch_mm,counts_per_mm,controlword_offset,statusword_offset,mode_offset,mode_display_offset,target_position_offset,actual_position_offset
```

- `slave`: SOEM이 스캔한 EtherCAT slave 순번
- `start_mm/end_mm`: 해당 stator가 담당하는 전역 mover 좌표 범위
- `coil_pitch_mm`: coil pitch 또는 segment pitch
- `counts_per_mm`: 드라이브 position unit 변환값
- `*_offset`: 해당 slave의 PDO image 안에서의 byte offset

PDO offset은 반드시 해당 LMS Slave Driver의 ESI 파일, 매뉴얼, 또는
`slaveinfo -map` 출력으로 확인해야 합니다. 제조사마다 PDO layout이 다릅니다.

## 5. 실시간성 전략

Windows 일반 커널만으로는 hard real-time을 보장하기 어렵습니다. 초기 개발과
시퀀스 검증은 1 ms~8 ms 주기로 시작하고, 양산 수준의 고속/고정밀 LMS 제어는
다음 중 하나로 격상하는 구조를 권장합니다.

- Windows + RTX64 같은 RTOS 확장
- Linux PREEMPT_RT/Xenomai + EtherLab 또는 SOEM
- 상용 EtherCAT Master/Soft Motion 런타임

이 저장소의 첫 목표는 공개 소스 기반 EtherCAT 통신, CiA 402 구동 골격,
LMS stator handoff 알고리즘을 독립적으로 확보하는 것입니다.
