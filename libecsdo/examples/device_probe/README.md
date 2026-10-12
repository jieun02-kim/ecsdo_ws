# device_probe: 장치 판별 진입 흐름과 LLM 입력용 JSON 덤프

슬레이브마다 SII와 OD를 읽어 판별 경로(branch)를 정하고, 그 과정을 로그로 남긴다.
그리고 LLM이 "어떤 객체나 PDO 엔트리가 어떤 역할인지"를 정하는 데 필요한 정보를
슬레이브당 JSON 파일 하나로 덤프한다. **LLM 호출은 하지 않는다.**

- 읽기 전용이다. 마스터를 예약하지 않고 `ecrt_master_activate()`도 호출하지 않는다. PRE-OP 상태면 충분하다.
- SDO abort는 치명적 오류로 다루지 않는다. `errors`에 기록하고 다음 항목으로 넘어간다.

## 빌드와 실행

명령어만 빠르게 보려면 [`RUN.md`](RUN.md)를 본다.

libecsdo 빌드(`ECSDO_BUILD_EXAMPLES=ON`, 기본값)에 포함되어 있다.

```sh
cmake -S libecsdo -B build -DIGH_SOURCE_DIR=<IgH 소스 경로>
cmake --build build
sudo ./build/device_probe                 # → ./dump/slave_<pos>.json
```

| 옵션 | 기본값 | 뜻 |
|---|---|---|
| `-m MASTER` | 0 | `/dev/EtherCAT<MASTER>` |
| `-o DIR` | `dump` | 출력 디렉터리 (없으면 만든다) |
| `-v FILE` | 소스의 `vendor_ids.json` | vendor_id → 이름 표 |
| `-t MS` | 30000 | 사전이 다 받아질 때까지 기다리는 최대 시간 (`ecsdo_wait_ready`) |

SDO upload에는 `/dev/EtherCAT<n>` 쓰기 권한이 필요하다. 권한이 없으면 첫 upload에서 멈추고 안내를 출력한다.
SII와 사전 읽기는 읽기 권한만 있어도 된다.

장치 없이 확인하려면 `ctest -R probe_sim`을 실행한다. 가짜 ioctl로 만든 버스(서보, I/O 터미널, 커플러,
MDP 장치, SDO Info 없는 서보)에 대해 실행되고, 결과는 `build/probe_sim_dump/`에 남는다.

## 분기 로직

```
1. 프로세스 데이터 (SII)        SII PDO 카테고리에 엔트리가 있는 PDO가 있거나,
                                SyncM 카테고리에 PD 타입(3/4) SM이 있으면 "있음"
     없음 ───────────────────▶ infrastructure   (1–5 덤프)
     SII를 못 읽음 ─────────▶ 판정 불가: errors에 기록하고 2로 진행
2. CoE (.coe)
     없음 ───────────────────▶ non_coe          (1–5 + sii.pdo)
3. SDO Info                     기록만 한다 (General 카테고리가 없으면 unknown)
4. 0x1000 upload                SDO Info 지원 여부와 관계없이 읽는다
     하위 16비트 402 ────────▶ cia402           (1–14, 필요하면 sii.pdo)
     5001 ───────────────────▶ mdp              (1–6)
     그 외, 또는 못 읽음 ───▶ other_profile    (1–6)
```

로그 예 (시뮬레이션):

```
[slave 0] "SimServo" vendor 0x00001234 product 0x00000001 rev 0x00000002, AL 0x02
  1. process data (SII): yes (assigned PDOs 2, PD SMs 2)
  2. CoE: yes
  3. SDO info (SII General): yes
  4. 0x1000 = 0x00020192, profile 402 -> cia402
  => cia402, 1 error(s) -> dump/slave_0.json
[slave 2] "SimCoupler" vendor 0x00000002 product 0x0000abce rev 0x00000000, AL 0x02
  1. process data (SII): no (assigned PDOs 0, PD SMs 0)
     -> infrastructure
```

## JSON 필드

키는 아래 순서로 나온다. 최상위에는 항상 `position`, `branch`, `errors`가 있다.
OD에서 읽지 못한 항목은 키를 생략하고, 이유는 `errors`에 남긴다.

| 순서 | 키 | 출처 | 형태 |
|---|---|---|---|
| 1 | `sii.identity` | `EC_IOCTL_SLAVE` (마스터가 파싱한 SII 0x0008–0x000D) | `{vendor_id, product_code, revision}` hex 문자열. serial 제외 |
| 2 | `sii.vendor_name` | `vendor_ids.json` | 문자열 또는 null |
| 3 | `sii.strings` | `EC_IOCTL_SLAVE` (General → Strings) | `{name, order, group}`, 빈 값은 null. cia402에서 0x1008을 읽었으면 `name`은 뺀다 |
| 4 | `sii.mailbox` | `EC_IOCTL_SLAVE` (SII 0x001C) | `["CoE", "FoE", ...]` |
| 5 | `sii.sdo_info` | General CoE Details | true / false / null (General 없음) |
| 6 | `od.device_type` | 0x1000 | `{raw, profile, type_code, type}`. `type`은 402일 때만 해석: frequency_converter / servo_drive / stepper_motor / multiple_axes. 그 외 프로파일은 `additional_info`(상위 16비트) |
| 7 | `od.identity_match` | 0x1018:01–04 | true / false / null (1–3번을 못 읽음) |
| 7′ | `od.identity_mismatch` | | 불일치할 때만: 다른 필드의 OD 값과 `serial_differs` |
| 8 | `od.name_ver` | 0x1008, 0x1009, 0x100A | `{device_name, hardware_version, software_version}`. 없는 값은 키 생략 |
| 9 | `od.drive_modes` | 0x6502 | `{raw, modes: ["PP","VL","PV","PT","HM","IP","CSP","CSV","CST"]}` |
| 10 | `od.rxpdo` | 0x1C12 → 0x16xx | `{basis: "assigned", pdos: [{index, entries: [...]}]}` |
| 11 | `od.txpdo` | 0x1C13 → 0x1Axx | 위와 같다 |
| 12 | `od.profile_axis0` | 사전 0x6000–0x67FF | `[{index, name, data_type, access}]` |
| 13 | `od.profile_axes_extra` | 사전 0x6800–0x9FFF | `[{axis, range, objects}]`, 객체가 있는 축만. 없으면 `[]` |
| 14 | `od.vendor` | 사전 0x2000–0x5FFF | `{objects: [{index, name}], total, truncated}`, 최대 150개 |
| – | `sii.pdo` | SII 원본의 TxPDO/RxPDO 카테고리 | non_coe: 항상. cia402: 10, 11번 중 못 읽은 방향만 |

**세부 규칙**

- **PDO 엔트리** (`od.rxpdo`/`od.txpdo`): `{index, subindex, bit_len, data_type, name}`.
  - 매핑 값은 32비트로, `index << 16 | subindex << 8 | bit_len`이다.
  - `name`과 `data_type`은 사전에서 대조한다. 레코드의 엔트리면 `entry_name`도 붙인다.
  - 사전에 없으면(SDO Info 없음) SII PDO의 같은 엔트리에서 가져오고 `"source": "sii"`를 붙인다.
  - gap(index 0)은 `{index: "0x0000", subindex, bit_len, gap: true}`로 나온다.
  - `basis: "assigned"`는 SII 기본값이 아니라 **현재 할당된 값**이라는 뜻이다.
- **`sii.pdo`**: `{rxpdo: [...], txpdo: [...]}`.
  - PDO마다 `{index, name, sm, entries: [{index, subindex, bit_len, data_type, name}]}` 형태다.
  - SII에서 SM이 할당된 PDO만 넣는다. SM 값이 0xFF인 PDO는 기본으로 할당되지 않은 것이라 뺀다.
  - PDO 이름(예: "Channel 1")은 넣는다. 디지털 I/O처럼 엔트리 이름이 모두 "Input"인 장치에서 채널을 구분할 수 있는 유일한 정보이기 때문이다.
- **객체 메타데이터** (12번):
  - subindex가 0 하나뿐인 객체는 그 값의 `data_type`과 `access`를 쓴다.
  - subindex가 여러 개인 객체는 `"data_type": "RECORD"`, `entries`(sub 1 이상의 개수), `sub1_type`, sub1의 `access`를 쓴다. 마스터가 VAR/ARRAY/RECORD 구분을 넘겨주지 않아서, ARRAY도 RECORD로 표시된다.
- **`access`**: `ro` / `rw` / `wo` / `none` 중 하나다. PREOP, SAFEOP, OP 상태마다 권한이 다르면 `ethercat sdos`처럼 `rwr-r-` 형식으로 쓴다.
- **12–14번을 쓸 수 없을 때**: `{"unavailable": 이유}`로 나온다. 이유는 `no_sdo_info`, `dict_not_ready`(대기 시간 초과), `dict_error` 중 하나다.
- **identity 비교**: vendor, product, revision은 항상 비교한다. serial은 SII 값이 0이 아닐 때만 비교한다. SII serial은 0인 장치가 많기 때문이다. serial 값 자체는 덤프하지 않는다.
- **`errors`**: `[{key, index, subindex, abort_code, message}]` 또는 `[{key, index, subindex, error}]`. `key`는 실패한 읽기가 속한 JSON 키다.
- **문자열**: 장치 문자열은 바이트열이다. 출력 가능한 ASCII가 아닌 바이트는 `\u00XX`로 기록한다.
- **제외 대상**: OD 0x0000–0x0FFF, 0x1001–0x10FF 중 8번 외의 것, 0x16xx/0x1Axx 객체의 이름, 할당되지 않은 PDO, 0x1C00/0x1C32/0x1C33, 0xA000–0xFFFF, 벤더 객체의 subindex.

## 예시 (시뮬레이션: `ctest -R probe_sim` 출력, 실제 장비가 아님)

cia402:

```json
{
  "position": 0,
  "branch": "cia402",
  "sii.identity": {"vendor_id": "0x00001234", "product_code": "0x00000001", "revision": "0x00000002"},
  "sii.vendor_name": null,
  "sii.strings": {"order": "SIM-SERVO-100", "group": "Servo Drives"},
  "sii.mailbox": ["CoE", "FoE"],
  "sii.sdo_info": true,
  "od.device_type": {"raw": "0x00020192", "profile": 402, "type_code": "0x02", "type": "servo_drive"},
  "od.identity_match": true,
  "od.name_ver": {"device_name": "SimServo", "hardware_version": "HW1.0"},
  "od.drive_modes": {"raw": "0x000003a5", "modes": ["PP", "PV", "HM", "CSP", "CSV", "CST"]},
  "od.rxpdo": {"basis": "assigned", "pdos": [
    {"index": "0x1600", "entries": [
      {"index": "0x6040", "subindex": 0, "bit_len": 16, "data_type": "UNSIGNED16", "name": "Controlword"},
      {"index": "0x607a", "subindex": 0, "bit_len": 32, "data_type": "INTEGER32", "name": "Target position"}
    ]}
  ]},
  "od.txpdo": {"basis": "assigned", "pdos": [
    {"index": "0x1a00", "entries": [
      {"index": "0x6041", "subindex": 0, "bit_len": 16, "data_type": "UNSIGNED16", "name": "Statusword"},
      {"index": "0x6064", "subindex": 0, "bit_len": 32, "data_type": "INTEGER32", "name": "Position actual value"},
      {"index": "0x0000", "subindex": 0, "bit_len": 8, "gap": true}
    ]}
  ]},
  "od.profile_axis0": [
    {"index": "0x6040", "name": "Controlword", "data_type": "UNSIGNED16", "access": "rw"},
    {"index": "0x607d", "name": "Software position limit", "data_type": "RECORD", "entries": 2, "sub1_type": "INTEGER32", "access": "rw"},
    {"index": "0x6502", "name": "Supported drive modes", "data_type": "UNSIGNED32", "access": "ro"}
  ],
  "od.profile_axes_extra": [{"axis": 1, "range": "0x6800-0x6FFF", "objects": 1}],
  "od.vendor": {"objects": [
    {"index": "0x2000", "name": "Motor ID"},
    {"index": "0x2100", "name": "Inertia Ratio"}
  ], "total": 2, "truncated": false},
  "errors": [
    {"key": "od.name_ver", "index": "0x100a", "subindex": 0, "abort_code": "0x06020000", "message": "Object does not exist in the object dictionary"}
  ]
}
```

(`od.profile_axis0`는 일부 생략)

non_coe:

```json
{
  "position": 1,
  "branch": "non_coe",
  "sii.identity": {"vendor_id": "0x00000002", "product_code": "0x0000abcd", "revision": "0x00100000"},
  "sii.vendor_name": "Beckhoff Automation GmbH & Co. KG",
  "sii.strings": {"name": "SimIn2", "order": "SIM-IN2", "group": "DigIn"},
  "sii.mailbox": [],
  "sii.sdo_info": false,
  "sii.pdo": {"rxpdo": [],
    "txpdo": [
      {"index": "0x1a00", "name": "Channel 1", "sm": 0, "entries": [
        {"index": "0x6000", "subindex": 1, "bit_len": 1, "data_type": "BOOLEAN", "name": "Input"}
      ]},
      {"index": "0x1a01", "name": "Channel 2", "sm": 0, "entries": [
        {"index": "0x6010", "subindex": 1, "bit_len": 1, "data_type": "BOOLEAN", "name": "Input"}
      ]}
    ]},
  "errors": []
}
```

## 정보 출처 요약

| 정보 | 출처 | 이유 |
|---|---|---|
| identity, name/order/group, mailbox, General 유무, SDO Info 비트 | `EC_IOCTL_SLAVE` (`ecsdo_read_dict`, `ecsdo_read_sii_info`) | 마스터가 이미 파싱하고 검증한 값이다. 예를 들어 mailbox 설정이 잘못되면 마스터가 0으로 만든다 |
| SII PDO, SyncM | `EC_IOCTL_SLAVE_SII_READ` → `ecsdo_sii_parse` | 마스터의 `EC_IOCTL_SLAVE_SYNC*`로는 얻을 수 없다. CoE 장치는 스캔 때 CoE에서 읽은 PDO로 덮어쓰이고, 엔트리 data_type은 파싱할 때 버려진다 |
| OD 값 | `ecsdo_upload` (raw, 이 프로그램에서 해석) | SDO Info가 없어도 읽을 수 있다 |
| 객체 이름, 타입, 권한 | `ecsdo_read_dict` | `ethercat sdos`와 같은 사전 |

`vendor_ids.json`에는 확인된 값만 넣는다. `_`로 시작하는 키는 무시된다.
