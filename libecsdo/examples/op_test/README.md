# op_test

libecsdo v2로 알아낸 정보만으로 버스를 OP까지 올리는 테스트. **제어하지 않는다**:
출력(Controlword 등)은 0으로 유지하고 입력만 읽는다.

## 흐름

기본은 libethercat, libethercat에 없는 것만 libecsdo.

1. **libecsdo** `ecsdo_wait_ready` — 마스터가 모든 사전을 받을 때까지 대기.
2. **libethercat** 마스터 예약, `ecrt_master` / `ecrt_master_get_slave`(슬레이브 수·정체성),
   `ecrt_master_get_sync_manager` / `_get_pdo` / `_get_pdo_entry`(현재 PDO 매핑).
3. **libecsdo** `ecsdo_get_items` — `wanted.conf`의 이름 → index:sub·타입·비트 길이 (현재 값도 SDO로 읽음).
4. **libethercat** `ecrt_master_slave_config`, 매핑에 있는 항목만 `ecrt_slave_config_reg_pdo_entry` → offset.
5. **`--activate`일 때만** activate, 출력 0으로 초기화, 1 ms 주기로 OP, 1초 유지, 프로세스 데이터를
   사전 타입으로 해석해 출력, 해제.

## 사용

```sh
# libecsdo 최상위에서 같이 빌드된다 (libethercat이 있을 때만, ECSDO_BUILD_OP_TEST=ON 기본)
cd ~/ecsdo_ws
cmake -S libecsdo -B build -DIGH_SOURCE_DIR=/home/jieun/ethercat
cmake --build build
cd build/examples/op_test       # wanted.conf가 복사되어 있음

# 원하는 항목 고르기: 전체 SDO 목록을 보고 이름 확인, 값 미리 읽어 보기
ecsdo_cli scan > dict.json
ecsdo_cli -p 1 get "Controlword" "Statusword" "Target Position" "Position Actual Value"

./op_test                 # 1~4만 (장치에 쓰기 없음)
./op_test --activate      # OP까지 (출력 0, 제어 없음)
./op_test -c other.conf   # 다른 항목 목록
```

`wanted.conf`: 한 줄에 이름 하나 (libecsdo selector 형식, `#` 주석).
매핑에 없는 항목, 모호하거나 없는 이름은 등록 전에 걸러지고 이유가 출력된다.

## 주의

- OP 전환 중 IgH 마스터가 장치의 현재 PDO 매핑을 **같은 값으로 다시 쓴다** (SDO download, IgH 동작).
- 사용자 공간 프로세스 데이터는 0으로 초기화되지 않으므로 activate 직후 `memset`으로 지운다.
- 다른 프로그램이 마스터를 쓰고 있으면 `ecrt_request_master`가 실패한다.
