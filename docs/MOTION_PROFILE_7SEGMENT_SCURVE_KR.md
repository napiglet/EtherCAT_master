# 7-Segment S-Curve Trajectory Planner

작성일: 2026-07-03

## 목적

상용 모션 마스터의 `S-Curve`는 단순히 가속도를 부드럽게 필터링하는 기능이 아니라,
목표 거리와 제한값을 기준으로 전체 이동 궤적을 먼저 계산하는 trajectory planner입니다.

이번 구현은 기존 `SCurve`/`JerkRatio`/`LMS` position move에 대해 다음 제한값을 사용해
정지 시작, 정지 종료 기준의 7-segment jerk-limited profile을 생성합니다.

- 이동 거리
- 최대 속도
- 최대 가속도
- 최대 감속도
- jerk 제한
- 제어 주기

## 7개 구간

정방향 이동 기준으로 속도/가속도/jerk는 다음 7개 구간으로 구성됩니다.

| Segment | 동작 | Jerk | Acceleration |
| --- | --- | --- | --- |
| 1 | 가속도를 0에서 +A까지 증가 | +J | 0 -> +A |
| 2 | 최대 가속도 유지 | 0 | +A |
| 3 | 가속도를 +A에서 0까지 감소 | -J | +A -> 0 |
| 4 | 정속 구간 | 0 | 0 |
| 5 | 감속도를 0에서 -D까지 증가 | -J | 0 -> -D |
| 6 | 최대 감속도 유지 | 0 | -D |
| 7 | 감속도를 -D에서 0까지 감소 | +J | -D -> 0 |

긴 거리에서는 7개 구간이 모두 나타납니다.
짧은 거리에서는 정속 구간 또는 정가속/정감속 구간이 0초가 될 수 있습니다.
그래도 내부 구조는 항상 7개 segment 기준으로 계산됩니다.

## 기존 방식과 차이

이전 구현은 `ramp_time`을 기준으로 매 cycle마다 가속도를 조금씩 따라가게 하는 방식이었습니다.
즉, 움직임은 부드러웠지만 전체 이동 거리를 기준으로 구간 시간이 먼저 계산되지는 않았습니다.

현재 구현은 다음 순서로 동작합니다.

1. 현재 위치와 목표 위치로 이동 거리 `S` 계산
2. 최대 속도 `V`, 가속도 `A`, 감속도 `D`, jerk `J` 결정
3. 최대 속도까지 올렸다가 감속하는 데 필요한 거리 계산
4. 거리가 충분하면 정속 구간 `Tv` 추가
5. 거리가 짧으면 binary search로 가능한 peak velocity를 낮춤
6. 7개 segment의 시작 시간, 시작 위치, 시작 속도, 시작 가속도, jerk 저장
7. 매 RT cycle마다 현재 segment의 3차 방정식으로 target position/velocity 계산

## 적용 코드 위치

- 선언:
  - `linux_rt/ethercat_rt_core/include/motion_cia402.h`
- 구현:
  - `linux_rt/ethercat_rt_core/src/motion_cia402.c`
- 테스트:
  - `linux_rt/ethercat_rt_core/test/motion_cia402_test.c`

핵심 함수:

- `scurve_build_position_plan()`
  - 목표 거리 기준으로 7개 segment 계획을 생성합니다.
- `step_position_profile_smooth()`
  - 생성된 계획을 매 cycle마다 평가해서 target PDO를 만듭니다.
- `scurve_phase_from_peak_velocity()`
  - 특정 peak velocity에서 가속/감속 phase의 시간과 거리를 계산합니다.

## 현재 적용 범위

현재 1차 구현은 position move의 정지 시작/정지 종료 profile입니다.

적용됨:

- CSP `MoveAbs`
- CSP `MoveRel`
- `SCurve`
- `JerkRatio`
- `LMS` profile type의 position move
- short move peak velocity 자동 축소
- 정속 구간 자동 계산
- 가속/감속 비대칭 지원

보수적으로 fallback하는 경우:

- 시작 시 실제 속도가 0이 아닌 경우
  - 급격한 command discontinuity를 피하기 위해 기존 ramp 방식으로 fallback합니다.

향후 제품화 단계에서 추가해야 할 항목:

- non-zero initial velocity / final velocity 지원
- 연속 이동 blending
- look-ahead
- velocity override
- emergency stop 전용 jerk-limited stop profile
- 단위 변환: pulse, mm, m/s, mm/s
- axis별 soft limit / following error / in-position window

## Linux 빌드

```bash
cd ~/EtherCAT_master
git pull
cmake --build build/linux-xenomai --target motion_cia402_test -j$(nproc)
./build/linux-xenomai/bin/motion_cia402_test

cmake --build build/linux-xenomai --target ethercat_igh_backend_server -j$(nproc)
```

