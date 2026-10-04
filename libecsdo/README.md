# libecsdo v2

IgH EtherCAT Master에서 **SDO 목록(사전) 읽기**와 **SDO 값 하나 읽기(upload)** 만 하는 C 라이브러리.

- 읽기 전용. 장치에 쓰지 않고, 마스터를 예약하지 않는다 (제어 프로그램이 마스터를 잡고 있어도 동작).
- `ethercat sdos`, `ethercat upload`와 같은 정보를 함수로 받는다.
- 값은 `ethercat upload`처럼 타입을 확인하고 숫자/문자열로 해석해 준다.
- 보조 모듈 `ecsdo_util`: SDO 목록 JSON 덤프, **이름으로 값 읽기**(제어 코드용 표). 핵심 라이브러리는 이 모듈 없이도 쓸 수 있다.

## API

| 함수 | 하는 일 |
|---|---|
| `ecsdo_open(&ctx, master)` / `ecsdo_close(ctx)` | `/dev/EtherCAT<n>` 열기/닫기, 버전 확인 |
| `ecsdo_slave_count(ctx, &n)` | 슬레이브 수 |
| `ecsdo_wait_ready(ctx, timeout_ms)` | 마스터가 사전을 다 받을 때까지 대기 (**사전 읽기 전에 한 번**) |
| `ecsdo_read_dict(ctx, pos, &slave)` / `ecsdo_free_slave(slave)` | 장치 정보 + SDO 목록 (`ethercat sdos`) |
| `ecsdo_upload_value(ctx, pos, index, sub, buf, size, &val, &abort)` | **타입 확인 + 값 읽기 + 해석** (`ethercat upload`) |
| `ecsdo_upload(ctx, pos, index, sub, buf, size, &len, &abort)` | 값 원본 바이트만 (`ethercat upload -t raw`) |
| `ecsdo_decode(type, data, len, &val)` | 원본 바이트를 타입대로 해석 (I/O 없음) |
| `ecsdo_type_name(type)`, `ecsdo_abort_string(code)` | 타입 이름 (`"UNSIGNED32"`), abort 코드 설명 |

**libethercat으로 하는 것 (이 라이브러리에 없음):** 슬레이브 수·정체성 `ecrt_master()` /
`ecrt_master_get_slave()`, 현재 PDO 매핑 `ecrt_master_get_sync_manager()` / `ecrt_master_get_pdo()` /
`ecrt_master_get_pdo_entry()`, 마스터 예약·PDO 등록·OP. libecsdo는 libethercat에 없는
**SDO 목록, 사전 준비 대기, 타입 해석 값 읽기, 이름으로 값 읽기**만 담당한다.
| `ecsdo_strerror(code)`, `ecsdo_last_errno(ctx)` | 오류 설명 |

## 예제

```c
#include <ecsdo/ecsdo.h>

ecsdo_ctx_t *ctx;
ecsdo_slave_t *s;
ecsdo_value_t v;
uint8_t buf[256];
uint32_t abort_code;

ecsdo_open(&ctx, 0);
ecsdo_wait_ready(ctx, 30000);            /* 사전이 다 받아질 때까지 */

ecsdo_read_dict(ctx, 1, &s);             /* SDO 목록 */
for (size_t i = 0; i < s->object_count; i++)
    printf("0x%04x %s\n", s->objects[i].index, s->objects[i].name);
ecsdo_free_slave(s);

/* 값: 사전에서 타입 확인 → upload → 해석 */
if (ecsdo_upload_value(ctx, 1, 0x1C12, 1, buf, sizeof(buf), &v, &abort_code)
        == ECSDO_OK && v.kind == ECSDO_VALUE_UNSIGNED)
    printf("RxPDO = 0x%04llx (%s)\n", (unsigned long long) v.u,
           ecsdo_type_name(v.data_type));          /* 0x1601 (UNSIGNED16) */

ecsdo_close(ctx);
```

`ecsdo_value_t`: `data_type`, `bit_length`, `kind`와 해석 결과 하나, 그리고 원본 `data`/`size`(항상 채워짐, `buf`를 가리킴).

| `kind` | 해석 결과 | 타입 |
|---|---|---|
| `ECSDO_VALUE_UNSIGNED` | `u` | BOOLEAN, BITn, UNSIGNEDn |
| `ECSDO_VALUE_SIGNED` | `i` | INTEGERn (부호 확장) |
| `ECSDO_VALUE_REAL` | `f` | REAL32/64 |
| `ECSDO_VALUE_STRING` | `str`, `str_len` (뒤 NUL 제거, NUL 종료 아님) | VISIBLE_STRING |
| `ECSDO_VALUE_RAW` | 없음 → `data`/`size` | OCTET/UNICODE_STRING, DOMAIN, TIME_* 등 |

타입 확인: 받은 크기가 타입 크기와 다르면 `ECSDO_ERR_TYPE_MISMATCH`(`ethercat upload`와 같은 규칙).
사전에 없는 항목(SDO Info 미지원 장치 등)은 `ECSDO_ERR_UNKNOWN_TYPE`과 원본 바이트를 돌려주므로,
타입을 알면 `ecsdo_decode()`로 해석한다. 둘 다 전송 자체는 성공한 경우다.

## 보조 모듈 `ecsdo_util` (JSON 덤프, 이름으로 값 읽기)

핵심 라이브러리 위에 얹은 선택 모듈이다. `#include <ecsdo/ecsdo_util.h>`, `libecsdo_util.a`와 `libecsdo.a`를 같이 링크한다.

| 함수 | 하는 일 |
|---|---|
| `ecsdo_dump_dict_json(ctx, master, FILE*)` | 모든 슬레이브의 SDO 목록을 JSON으로 (`ecsdo_cli scan`) |
| `ecsdo_get_items(ctx, items, n)` | 이름(selector)으로 찾아서 upload + 타입 확인 + 해석 → `items[]`에 채움 |
| `ecsdo_items_json(items, n, FILE*)` | 읽은 값 표를 JSON으로 (`ecsdo_cli get -j`) |
| `ecsdo_util_strerror(code)` | 아래 추가 오류 포함 설명 |

**제어 코드에서 쓰는 법**

```c
ecsdo_item_t items[] = {
    {.position = 1, .selector = "Device Type"},
    {.position = 1, .selector = "Supported Drive Modes"},
    {.position = 1, .selector = "Output Sync Manager Parameter/Minimum cycle time"},
    {.position = 1, .selector = "RxPDO(SM2) Assignment:1"},
};
ecsdo_get_items(ctx, items, 4);            /* 실패한 개수 반환, 0 = 전부 성공 */

int csp = items[1].result == ECSDO_OK
          && ((items[1].value.u >> 7) & 1);          /* CiA 402 bit 7: CSP */
uint32_t min_cycle_ns = (uint32_t) items[2].value.u;   /* 200000 */
uint16_t rx_pdo = (uint16_t) items[3].value.u;         /* 0x1601 */
```

각 항목에 `result`, `index`/`subindex`, `object_name`, `description`, `data_type`, `bit_length`, 권한,
`value`(해석된 값)가 채워진다. `value`는 항목 안의 `buf`를 가리키므로 항목을 복사하지 말 것.
전체 예: `examples/startup_check.c` (드라이브마다 프로파일, CSP 지원, 최소 주기를 확인).

**selector 형식** (영문 대소문자, 공백, `_`, `-` 무시)

| 형식 | 예 | 뜻 |
|---|---|---|
| 객체 이름 | `"Controlword"` | 항목이 하나뿐인 객체 |
| 객체 이름`:`sub | `"Identity Object:1"` | 레코드/배열의 sub |
| 객체 이름`/`항목 설명 | `"Identity Object/Vendor ID"` | 레코드의 항목 |
| 항목 설명 | `"Vendor ID"` | 일치하는 객체 이름이 없을 때 전체 항목에서 찾음 |
| index | `"0x1018:1"`, `"0x1000"` | 사전 없이도 됨 (사전이 있으면 타입도 사용) |

여러 개가 일치하면 고르지 않고 `ECSDO_ERR_AMBIGUOUS`(예: `"Identity Object"`, 두 PDO 할당 객체에 같은 설명).
추가 오류: `ECSDO_ERR_NOT_FOUND`, `ECSDO_ERR_AMBIGUOUS`, `ECSDO_ERR_NO_DICT`(사전 없는 장치 → index로 지정),
`ECSDO_ERR_BAD_SELECTOR`, `ECSDO_ERR_WRITE`.

**JSON 형식**

- `ecsdo-dict`: `{"format": "ecsdo-dict", "version": 1, "master": 0, "slaves": [{position, vendor_id, ..., "objects": [{"index": "0x1000", "name", "entries": [{"subindex", "data_type", "type_name", "bit_length", "read_access": [PREOP, SAFEOP, OP], "write_access", "description"}]}]}]}`
- `ecsdo-items`: `{"format": "ecsdo-items", "version": 1, "items": [{"position", "selector", "result", "error", "index", "subindex", "object_name", "description", "data_type", "type_name", "bit_length", "abort_code", "value_hex", "value"}]}`
- 장치 문자열은 바이트열이며 출력 가능한 ASCII가 아닌 바이트는 `\u00XX`로 기록된다 (Python: `s.encode("latin-1")`).

```sh
ecsdo_cli scan > dict.json                                  # SDO 목록 전체
ecsdo_cli get "Device Type" "Vendor ID" "RxPDO(SM2) Assignment:1"     # 모든 슬레이브, 표
ecsdo_cli -p 1 get -j "Supported Drive Modes" > values.json             # 한 슬레이브, JSON
```

## 결과 구조

```
ecsdo_slave_t   position, alias, vendor_id, product_code, revision, serial, name,
                al_state, coe, sdo_info, status, objects[]
 └ ecsdo_object_t   index, max_subindex, name, entries[]
    └ ecsdo_entry_t    subindex, data_type, bit_length,
                       read_access[3], write_access[3] (PREOP, SAFEOP, OP), description
```

| `status` | 의미 |
|---|---|
| `ECSDO_SLAVE_OK` | 사전 정상 |
| `ECSDO_SLAVE_NO_COE` | CoE 없음 → 사전·upload 없음 |
| `ECSDO_SLAVE_NO_SDO_INFO` | 사전 없음. upload는 가능 (index를 알고 있어야 함) |
| `ECSDO_SLAVE_NOT_READY` | 아직 스캔 전이거나 사전이 덜 받아짐 → `wait_ready` 후 다시 |
| `ECSDO_SLAVE_ERROR` | ioctl 실패 (`sys_errno`) |

upload 반환값: `OK`, `ERR_ABORT`(장치가 거부, `abort_code` → `ecsdo_abort_string()`),
`ERR_BUFFER_SIZE`(버퍼를 키워 다시), `ERR_NO_SLAVE`, `ERR_NOT_WRITABLE`, `ERR_IOCTL`,
(`upload_value`만) `ERR_TYPE_MISMATCH`, `ERR_UNKNOWN_TYPE`.
항목당 약 10 ms 걸리므로 실시간 루프에서 쓰지 말 것.

## 빌드와 설치 (작업 공간 `ecsdo_ws`)

```
ecsdo_ws/
  libecsdo/   소스 (이 디렉터리)
  build/      빌드 (git 제외)
  install/    설치 (git 제외): include/ecsdo/*.h, lib/libecsdo*.a, lib/cmake/ecsdo/, bin/ecsdo_cli
```

```sh
git clone https://github.com/jieun02-kim/ecsdo_ws.git
cd ecsdo_ws
# <IgH 소스 경로>: ./configure를 마친 IgH EtherCAT Master 소스 트리 (master/ioctl.h, config.h 필요)
cmake -S libecsdo -B build -DIGH_SOURCE_DIR=<IgH 소스 경로> \
      -DCMAKE_INSTALL_PREFIX=$PWD/install
cmake --build build
(cd build && ctest)          # 장치 없이 테스트 (ctest -T memcheck: valgrind)
cmake --install build
```

다른 프로젝트에서 사용:

```cmake
find_package(ecsdo 2 REQUIRED CONFIG)
target_link_libraries(my_app PRIVATE ecsdo::ecsdo_util ecsdo::ecsdo)
```
```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<ecsdo_ws 경로>/install
```

IgH를 업데이트하면 다시 빌드할 것 (ioctl 버전이 다르면 `ecsdo_open`이 `ECSDO_ERR_VERSION`).
값 upload에는 `/dev/EtherCAT0` 쓰기 권한이 필요하다 (사전 읽기는 읽기 권한만).

## 확인용 CLI `ecsdo_cli`

```sh
ecsdo_cli -p 1 sdos                       # = ethercat sdos -p 1
ecsdo_cli -p 1 upload 0x1000 0            # = ethercat upload -p 1 0x1000 0  → 0x00020192 131474
ecsdo_cli -p 1 upload -t raw 0x1000 0     # = ethercat upload -p 1 -t raw 0x1000 0
ecsdo_cli scan > dict.json                # 모든 슬레이브 SDO 목록 → JSON
ecsdo_cli get "Device Type" "Vendor ID"   # 이름으로 실제 값 (모든 슬레이브)
```

## 파일

```
include/ecsdo/ecsdo.h   공개 API
src/ecsdo.c             구현 (열기, 대기, 사전 읽기, upload)
src/ecsdo_types.c       CoE 타입 표 (CiA 301 / ETG.1000.6), 값 해석, abort 설명
src/ecsdo_ioctl.[ch]    ioctl 6종 (MODULE, MASTER, SLAVE, SLAVE_SDO, SLAVE_SDO_ENTRY,
                        SLAVE_SDO_UPLOAD). SDO_ENTRY는 목록 위치/index 두 방식으로 사용.
                        IgH 헤더는 여기서만 사용
include/ecsdo/ecsdo_util.h  보조 모듈 API
util/ecsdo_util.c       보조 모듈: JSON 덤프, selector, 이름으로 값 읽기
examples/ecsdo_cli.c    확인용 CLI (sdos, upload, scan, get)
examples/startup_check.c 제어 코드 시작 시 사용 예
examples/op_test/       libecsdo + libethercat으로 OP까지 (libethercat 있을 때만 빌드)
tests/                  가짜 ioctl 계층으로 하는 단위 테스트
```
