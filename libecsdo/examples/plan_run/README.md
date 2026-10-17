# plan_run

`tools/ecat_rules`가 만든 PDO 매핑 계획(`plans/slave_N.plan.json`)을 그대로 적용해 버스를 OP까지 올리고,
장치 형태에 따라 작은 동작을 바로 해 본다. 코드에 Vendor ID나 PDO 맵을 넣지 않는다.

| device_class | 동작 |
| --- | --- |
| servo_drive | CiA402 Enable → 0.5초 유지 → 작은 동작 → 0.5초 유지 → 동작 … 반복, 멈출 때 Disable |
| digital_output | 채널 순서대로 하나씩 켜기 (300 ms 간격) 반복, 멈출 때 모두 끔 |
| digital_input | 0.5초마다 입력 출력 |
| infrastructure (EK1100 등) | 설정만 (PDO 없음) |
| 그 외 / plan status가 matched가 아님 | 출력은 0으로 두고 입력만 출력 |

Ctrl+C(또는 `-t`초)까지 계속 돈다. 드라이브 동작은 계획의 운전 모드(`rule`: `cia402/csp` 등)를 따른다.

| 모드 | 동작 (기본값) |
| --- | --- |
| csp (추천) | 목표 위치 +200000 count까지 갔다가 돌아옴, 한 방향 2.5초 (cosine), 전체 약 5.3초 |
| csv | 속도 +5000 count/s 반파 사인 → -5000 반파 사인 (한 방향 2.5초) |
| cst | 토크 +1% 100 ms → 쉼 → -1% 100 ms. 시험하는 동안 0x6072 Maximum Torque를 2%로 제한하고 끝나면 원래 값으로 되돌림 |

## 전체 흐름 (한 번에)

```sh
cmake --build build                                   # build/examples/plan_run/plan_run
libecsdo/examples/plan_run/run_test.sh                # csp (기본)
libecsdo/examples/plan_run/run_test.sh --mode cst     # rule_engine 기본 모드
libecsdo/examples/plan_run/run_test.sh --amp 100000 --no-motion
```

`run_test.sh`는 Ctrl+C까지 도는 감시 루프다. 랜선을 뽑았다 꽂거나 다른 장치로 바꿔도 멈추지 않는다.

```
┌▶ 0. 장치 대기: `ethercat slaves` 목록이 2초 동안 그대로일 때까지
│  1. device_probe -o dump (+ chown)
│  2. rule_engine.py --mode <mode> --out plans
│     (needs_llm: true인 계획이 있을 때만 llm_step.py. 후보 rule은 사람이 확인해야 하므로 plan_run은 그 장치를 동작시키지 않음)
│  3. plan_run [옵션] plans/*.plan.json   → OP, 동작 반복
│        랜선 뽑힘 / 응답 슬레이브 수 변화 / OP 이탈 → 드라이브 Disable, 출력 0, exit 3
└─ exit 3: 처음으로 (새로 꽂힌 장치로 다시 덤프·매핑)
   그 밖의 실패: 장치 구성이 바뀔 때까지 기다렸다가 처음으로
   Ctrl+C: 드라이브 Disable, 출력 0 후 스크립트 종료
```

plan_run 종료 코드: 0 = Ctrl+C 또는 `-t` 시간 끝, 3 = 버스 바뀜(다시 덤프), 1 = 그 밖의 실패.

L7N + EK1100 + EL2004에서는 LLM 단계가 필요 없다 (세 계획 모두 `needs_llm: false`).

## plan_run만 따로

```sh
./build/examples/plan_run/plan_run -n plans/*.plan.json        # 계획 확인만 (마스터 안 씀)
sudo ./build/examples/plan_run/plan_run plans/*.plan.json      # 적용 + OP + 동작 (확인 질문 없이 바로 움직임)
sudo ./build/examples/plan_run/plan_run --no-motion plans/*.plan.json   # 드라이브 Enable/유지만
```

| 옵션 | 내용 |
| --- | --- |
| `-n`, `--dry-run` | 계획을 읽어 설정 내용만 출력 |
| `--no-motion` | 드라이브는 Enable 후 유지만 (Ctrl+C까지) |
| `-t SEC` | 시험 시간. 지나면 Disable/출력 0 후 종료 (기본: Ctrl+C까지) |
| `--amp N` | csp 이동량, count (기본 200000) |
| `--vel N` | csv 최고 속도, count/s (기본 5000) |
| `--torque N` | cst 토크, 0.1% 단위 (기본 10, 최대 50) |
| `--move-ms N` | 한 방향 동작 시간 (기본 2500) |
| `--guard N` | 시작 위치에서 최대 이동량, csp 추종 오차 한계 (기본 2 × amp) |
| `--speed-guard N` | 최대 속도, count/ms, 10 ms 평균 (기본: 계획 최고 속도 × 3, 최소 60). Enable 직후 200 ms는 검사 안 함 |
| `--dc`, `--dc-assign HEX` | 드라이브 DC 켜기 (SYNC0 1 ms) / AssignActivate 값 (기본 0x0300, L7N ESI 값). 기본은 DC 없음 |

## 어떻게 매핑하나

- 계획의 `rx`/`tx`를 PDO 번호별로 묶어 `ecrt_slave_config_pdos()`로 넘긴다. CoE 장치는 IgH가 PREOP에서
  0x1C12/0x1C13/0x16xx/0x1Axx를 직접 쓴다 (계획의 `sdo_writes` 중 PDO 관련 부분과 같은 내용).
- `sdo_writes` 중 PDO 관련이 아닌 것(0x6060 운전 모드 등)은 `ecrt_slave_config_sdo*()`로 넣는다 (OP 전환 때마다 씀).
- Sync Manager 번호는 마스터 스캔에서 정한다: 그 방향의 PDO가 이미 있는 SM, 없으면 그 방향의 첫 프로세스 데이터 SM.
  (EL2004처럼 메일박스 없는 단자는 SM0이 출력이다. `-n`은 버스를 안 보므로 SM2/SM3으로 표시한다.)
- 역할(`role`)로 제어 대상을 찾는다: `control_word`, `status_word`, `target_position`, `actual_position`,
  `digital_output[k]` 등. 다축은 `control_word[k]`.

## 안전 장치

- 계획과 버스가 다르면 (슬레이브 수, 위치별 Vendor ID/Product Code) 아무것도 설정하지 않고 끝낸다. 장치를 바꿨으면 1단계부터 다시.
- 모든 슬레이브가 OP이고 working counter가 정상일 때까지 출력은 0 (Controlword 0 = Disable).
- Enable 전까지 목표 위치 = 현재 위치. Enable한 순간의 위치를 기준으로 움직인다.
- 매 주기 검사, 하나라도 걸리면 바로 Disable: Fault, Operation enabled 해제, 기준에서 `--guard` 이상 이동,
  `--speed-guard` 이상 속도, csp 추종 오차.
- Fault는 Enable 단계에서 최대 3번 리셋을 시도한다. Enable이 5초 안에 안 되면 포기.
- Ctrl+C: 드라이브를 Disable하고 출력을 0으로 만든 뒤 끝냄. 두 번 누르면 즉시 종료.
- 링크가 끊기거나 응답 슬레이브 수가 50 ms 넘게 다르거나, OP에서 100 ms 넘게 벗어나면 정지 절차 후 exit 3 (run_test.sh가 다시 덤프).
  케이블이 빠진 동안은 명령이 장치에 닿지 않으므로 드라이브·단자는 자체 통신 감시(SM watchdog)로 멈춘다.

## 주의

- count 단위는 엔코더 분해능에 따라 실제 각도가 다르다. 처음엔 기본값으로 돌려 보고
  출력되는 `peak travel`을 보고 `--amp`를 조절한다 (L7N: 0x2002 Encoder Resolution 참고).
- cst는 부하 없는 축에서 토크만큼 계속 가속한다. 기본값은 작게 잡았고 이동/속도 가드가 있지만 csp를 먼저 쓴다.
- DC는 기본으로 끈다. L7N 4대 버스에서 `Failed to calculate bus topology` 후 슬레이브마다 "did not sync after 5000 ms"로
  OP가 늦었다 (DC 없이는 OP 정상). `--dc`를 쓰면 OP 대기 시간을 드라이브 수 × 6초만큼 늘린다.
- 다른 프로그램이 마스터를 쓰고 있으면 `ecrt_request_master`가 실패한다.
