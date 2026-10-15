# EtherCAT 장치 rule 엔진 (프로토타입, rule v2)

device_probe 덤프 JSON을 읽어 장치 형태별 rule을 적용하고, 매핑 요청(mode, extras)에 맞는 PDO 매핑 계획을 JSON으로 만든다.
rule이 못 푼 장치는 로컬 LLM(Ollama)으로 판별해 모델 rule 후보를 만든다.
버스에는 아무것도 쓰지 않는다. 매핑을 바꿔야 하면 실행할 SDO 쓰기 순서(`sdo_writes`)만 계획에 담는다.

```
device_probe (C, libecsdo)  ──▶ dump/slave_N.json
                                    │
rule_engine.py              ──▶ plans/*.plan.json          (rule로 풀리면 여기서 끝)
                                    │ needs_llm인 장치만
llm_step.py (Ollama)        ──▶ llm_out/*.llm.json + *.candidate.yaml (모델 rule 후보)
                                    │ 사람이 확인 후 rules/models/로 옮기고 approved: true
rule_engine.py              ──▶ 같은 모델은 다음부터 LLM 없이 rule로 처리
```

rule 파일 형식과 매핑 결정 순서는 [rules/README.md](rules/README.md)에 있다.

## 구성

```
rule_engine.py               rule 엔진 (Python 3.10+, PyYAML)
llm_step.py                  needs_llm 장치를 Ollama로 판별 → 모델 rule 후보
rules/infrastructure.yaml    커플러, 확장 모듈 (PDO 없음)
rules/non_coe_io.yaml        CoE 미지원 I/O (SII PDO 그대로)
rules/cia402.yaml            CiA402 드라이브: 모드별(cst, csp, csv) 필수 항목 + extras
rules/mdp5001.yaml           MDP 모듈형 I/O
rules/cia401.yaml            CiA401 범용 I/O
rules/models/                모델 rule (VID + 0x1008). '_'로 시작하는 파일, approved: false인 후보는 읽지 않음
rules/llm_vocabulary.yaml    LLM 출력 선택지 (형태, 역할, 방향 규칙)
samples/                     device_probe 출력 형식의 덤프 10개 (가상 9개 + probe_sim F/T 센서)
tests/                       단위 테스트 (LLM 단계는 Ollama 없이 가짜 응답으로 테스트)
```

## 실행

```bash
pip install pyyaml
python3 rule_engine.py dump/*.json --out plans                                   # 요청 없음 = default (cst, extras 없음)
python3 rule_engine.py dump/*.json --mode csp --extras state_monitor,error_detect --out plans
python3 rule_engine.py --vocab                                                   # LLM 출력 선택지 JSON
python3 llm_step.py dump/*.json --out llm_out                                    # needs_llm 장치를 Ollama로 판별
python3 llm_step.py dump/*.json --out llm_out --dry-run                          # LLM 입력만 저장 (호출 안 함)
python3 -m unittest discover -s tests
```

입력은 device_probe 출력(`"sii.mailbox"` 같은 평평한 키)이다. 엔진은 읽을 때 내부 형식으로 바꾼다(`normalize_dump`).

`--mode`는 CiA402 운전 모드, `--extras`는 rule의 extras 이름이다. rule에 없는 이름은 무시하고 `missing_extras`에 보고한다.

## rule 선택

| 조건 | 적용 rule |
| --- | --- |
| branch = infrastructure | `infrastructure` → `no_process_data` |
| branch = cia402 또는 0x1000 profile = 402 | `cia402` |
| branch = mdp 또는 profile = 5001 | `mdp5001` |
| profile = 401 | `cia401` |
| PDO 없음 | `infrastructure` |
| CoE 미지원 (SII mailbox에 CoE 없음), PDO 있음 | `non_coe_io` |
| 그 외 | rule 없음 → `fallback` |

선택한 rule에 맞는 모델 rule(`models/*.yaml`)이 있으면 그 위에 덮어쓴다.
매칭은 Vendor ID + 0x1008 Device Name이고, 0x1008이 없으면 SII 주문번호와 비교한다. `match.product_code`가 있으면 그것도 비교한다.
`base_rule`이 선택한 rule과 같은 모델만 쓴다. `base_rule`이 없는(null) 모델은 프로파일 rule이 없는 장치용이고,
`required`의 index(와 sub)로 역할을 붙인다. 이때 rule 이름은 `model:<파일>`이다.

## LLM 단계 (llm_step.py)

| 경우 | LLM이 정하는 것 | 후보 |
| --- | --- | --- |
| fallback (프로파일 rule 없음) | 1) 장치 형태 2) 그 형태의 역할 안에서 엔트리별 역할 | `base_rule: null` 모델 rule |
| rule 적용, 분류 못 한 엔트리 있음 | 남은 엔트리의 역할 | 그 rule을 `base_rule`로 한 모델 rule |
| cia402, 필수 객체 없음 | 빠진 역할마다 대체할 벤더 객체 또는 none | 모드 필수 목록을 바꾼 모델 rule |

- **모델:** 기본 모델은 `qwen3.5:4b`다(`--model`, `--host`로 바꿀 수 있음). temperature 0, seed 42, thinking 끔.
- **출력 강제:** JSON 스키마로 선택지(`llm_vocabulary.yaml`, rule의 역할, 덤프의 벤더 객체 index) 안의 값만 나오게 강제한다.
- **입력:** 장치 정보, 판단할 PDO 엔트리, 선택지만 넣는다. 덤프 전체를 넣지 않는다. 객체 이름은 참고용으로 최대 60개만 넣는다.
- **검증:** 방향 규칙에 어긋난 역할은 코드가 `unknown`으로 돌리고 `problems`에 남긴다.
- **후보 저장:** 후보는 `source.approved: false`로 저장된다. 확인한 뒤 `true`로 바꾸고 `rules/models/`로 옮겨야 엔진이 읽는다.
- **이름이 없는 장치:** 이름(0x1008, SII 주문번호)이 없으면 후보의 `match`는 Vendor ID + product code다.

## 이전 엔진(v1)과 달라진 점

- 선택 객체를 장치에 있다고 자동으로 넣지 않는다. 요청한 extras만 넣는다.
- 매핑 방법을 CoE Details(`sii.coe_details`)로 정한다.

  | `pdo_config` (bit 3) | `pdo_assign` (bit 2) | action |
  | --- | --- | --- |
  | true | - | `remap`: 필수 → extras 순으로 새 매핑 |
  | false | true | `select_fixed`: 필요한 객체를 덮는 고정 PDO 세트(`sii.pdo`)를 골라 0x1C12/0x1C13만 씀 |
  | false | false | `keep_current`: 현재 매핑 그대로. 필수 객체가 없으면 실패 |
  | 덤프에 없음 | | 현재 매핑에 선택 객체가 모두 있으면 `keep_current`, 아니면 `remap` (notes에 확인 필요 표시) |

- `on_event` 객체(0x1001 등)는 PDO에 넣지 않고 `on_event` 목록에 SDO 조회 대상으로 남긴다.
- 운전 모드(0x6060)는 rule의 `init_sdo`대로, PDO에 없으면 초기화 때 SDO로 쓴다. 다축이면 축마다 쓴다.
- I/O의 상태 비트(Underrange, Error, TxPDO State 등)는 `channel_status`를 요청했을 때만 채널로 등록한다.
  요청하지 않아도 디지털 채널로 잘못 분류하지 않도록 채널 패턴에서는 뺀다.
- 계획의 역할이 `llm_vocabulary.yaml`의 방향 규칙과 맞지 않으면 notes에 경고를 남긴다.

## 출력 (plan JSON)

| 키 | 내용 |
| --- | --- |
| status | matched / fallback / failed / no_process_data |
| rule, model_rule | 적용한 rule (예: cia402/cst, mdp5001), 덮어쓴 모델 rule 파일 |
| request | 실제로 적용한 요청 (mode, extras) |
| device_class, count | 장치 형태, 축 또는 채널 수 |
| action | keep_current / keep_sii / remap / select_fixed / none |
| rx, tx | 엔트리별 PDO, index, subindex, bit_len, bit_offset, role |
| rx_bits, tx_bits | 방향별 프로세스 데이터 크기 |
| channels | I/O 장치의 채널 목록 |
| on_event | 필요할 때 SDO로 읽는 객체 |
| sdo_writes | remap, 고정 세트 할당, 모드 설정에 필요한 SDO 쓰기 순서 |
| missing_required | 없는 필수 객체 |
| missing_extras | 요청했지만 넣지 못한 extras 객체와 이유 |
| record | 인프라 장치의 토폴로지 정보 |
| needs_llm | LLM 판별이 필요한지 |
| notes | 판단 근거와 확인 필요 사항 |

## 덤프에서 쓰는 항목

| 항목 | 용도 |
| --- | --- |
| `sii.coe_details` | 매핑 방법 결정 (remap / select_fixed / keep_current) |
| `sii.pdo` (`"sm": null` = 기본 할당 안 됨) | 현재 매핑(할당된 것), `select_fixed` 후보(전부) |
| `od.rxpdo` / `od.txpdo` | 현재 매핑. 한 방향을 못 읽었으면 그 방향만 SII 기본 PDO |
| `od.profile_axis0`, `od.profile_axes_extra` | cia402 객체 존재 확인 |
| `od.objects` (other_profile), `od.vendor` (cia402) | LLM 입력 (객체 이름, 대체 객체 후보) |

0x1C12 / 0x16xx의 접근 권한은 아직 덤프하지 않는다 (remap 가능 여부 교차 확인용으로 있으면 좋음).

## 한계

- `samples/`는 형식 확인용 가상 데이터와 시뮬레이션 출력이다. 실제 L7N과 Beckhoff 덤프로 다시 검증해야 한다.
- LLM 대체 객체 후보는 벤더 객체의 타입·비트 길이를 모른다 (`od.vendor`에 이름만 있음). 표준 객체 값을 넣고 확인 필요로 표시한다.
- rule에 `remap`이 없으면 PDO 번호는 0x1600/0x1A00, 축 간격 0x10을 쓴다. 다축 장치는 모델 rule로 지정해야 한다.
- `select_fixed`는 고정 PDO 세트 사이의 배타 조건(SII Exclude)을 확인하지 않는다.
- remap 시 SM 크기 제한은 확인하지 않는다.
- 0x1001 같은 on_event 객체는 존재 여부를 확인하지 않는다 (SDO 조회 때 확인).
