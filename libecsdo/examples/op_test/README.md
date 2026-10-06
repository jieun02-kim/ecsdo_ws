# op_test

libecsdo v2로 알아낸 정보만으로 버스를 OP까지 올리는 테스트. **제어하지 않는다**:
출력(Controlword 등)은 0으로 유지하고 입력만 읽는다.

## 흐름

기본은 libethercat, libethercat에 없는 것만 libecsdo.

1. **libecsdo** `ecsdo_wait_ready` — 마스터가 모든 사전을 받을 때까지 대기.
2. **libethercat** 마스터 예약, `ecrt_master` / `ecrt_master_get_slave`(슬레이브 수·정체성).
3. **libecsdo** `ecsdo_get_items` — `pdo.conf`의 이름 → index:sub·타입·비트 길이·접근 권한
   (현재 값도 SDO로 읽음). 이것으로 PDO 매핑을 만든다: OP에서 쓰기 가능 → RxPDO 0x1600 (SM2, 출력),
   아니면 TxPDO 0x1A00 (SM3, 입력).
4. **libethercat** `ecrt_master_slave_config`, `ecrt_slave_config_pdos`(3의 매핑),
   `ecrt_slave_config_reg_pdo_entry` → offset.
5. **`--activate`일 때만** activate, 출력 0으로 초기화, 1 ms 주기로 OP, 1초 유지, 프로세스 데이터를
   사전 타입으로 해석해 출력, 해제.

## 사용

```sh
# libecsdo 최상위에서 같이 빌드된다 (libethercat이 있을 때만, ECSDO_BUILD_OP_TEST=ON 기본)
cd ecsdo_ws
cmake -S libecsdo -B build -DIGH_SOURCE_DIR=<IgH 소스 경로>
cmake --build build
cd build/examples/op_test       # pdo.conf, sdo.conf가 복사되어 있음

# 원하는 항목 고르기: 전체 SDO 목록을 보고 이름 확인, 값 미리 읽어 보기
ecsdo_cli scan > dict.json
ecsdo_cli -p 1 get "Controlword" "Statusword" "Target Position" "Position Actual Value"

./op_test                 # 1~4만 (장치에 쓰기 없음)
./op_test --activate      # OP까지 (출력 0, 제어 없음)
./op_test --pdo other.conf   # 다른 PDO 항목 목록
# 매 실행마다 2~4단계에서 읽고 만든 것을 ./op_test.json에 저장 (activate 전, 덮어씀)
./op_test --save read.json   # 다른 파일로
./op_test --no-save          # 저장 안 함
./op_test --sdo other.conf   # 식별용 SDO 목록 (기본 sdo.conf, 없으면 건너뜀)
./op_test --sine 0        # 축 0만 사인파 (CSP, 0.25 rev, 주기 10 s, 3주기)
./op_test --sine 0,1,3    # 여러 축
```

`sdo.conf`: 시작할 때 SDO로 한 번 읽는 장치·모터 식별 값 (Device Type/Name, 모터 ID, 엔코더,
정격, 보호 설정 등). 매핑하지 않고 출력과 저장 JSON의 `"sdo_values"`에만 쓴다.

`pdo.conf`: 한 줄에 이름 하나 (libecsdo selector 형식, `#` 주석).
모호하거나 없는 이름, 64비트 넘는 항목은 매핑 전에 걸러지고 이유가 출력된다.
줄 순서대로 PDO에 들어간다. L7N의 0x1600/0x1A00은 각각 최대 10개.

## 주의

- `--activate` 시 PREOP에서 IgH 마스터가 **3에서 만든 매핑을 장치에 쓴다** (0x1C12/0x1C13, 0x1600/0x1A00,
  SDO download). 장치의 기존 SM2/SM3 매핑을 대체한다 (0x1010 저장은 하지 않음).
- PDO에 넣을 수 없는 오브젝트가 있으면 장치가 SDO abort로 거부하고 그 슬레이브는 PREOP에 머문다
  (`dmesg`에 원인). IgH가 사전의 PDO 매핑 가능 비트를 알려주지 않아 미리 거를 수 없다.
- 사용자 공간 프로세스 데이터는 0으로 초기화되지 않으므로 activate 직후 `memset`으로 지운다.
- 다른 프로그램이 마스터를 쓰고 있으면 `ecrt_request_master`가 실패한다.

## --sine (제어)

- 선택한 축만 CiA402 상태 머신으로 enable (0x06 → 0x07 → 0x0F). enable 전에는 Target Position =
  Position Actual Value로 보내 점프가 없고, enable 순간의 실제 위치가 시작 위치가 된다.
- 모든 선택 축이 enable되면 `시작 위치 + A·env(t)·sin(2πt/T)`. env는 첫 주기에 0→1, 마지막 주기에 1→0.
  끝나면 시작 위치에서 Shutdown(0x06).
- counts/rev = 2^"Encoder Resolution"(SDO, L7N 0x2002). 위치 단위가 엔코더 카운트라는 가정
  (Electric Gear Mode 0). 모드는 시작 SDO로 0x6060 = 8 (CSP).
- 정지 조건 → Quick stop(0x02)을 0.5 s 보낸 뒤 해제: fault 비트, Operation enabled 이탈,
  추종 오차 > 0.1 rev (드라이브 0x6065는 2,000,000 counts로 커서 별도 감시), working counter 불완전, Ctrl+C.
  5 s 안에 모든 축이 enable되지 않으면 Shutdown.
- 진폭·주기·한계는 `op_test.c` 상단 `SINE_*`, `MAX_FOLLOW_REV`.
