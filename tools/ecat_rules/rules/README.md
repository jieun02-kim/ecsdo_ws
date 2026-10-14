# rule 파일 (v2)

장치 형태별로 PDO 매핑 규칙을 정의한다. 모든 rule은 같은 구성을 따른다.

| 구성 | 의미 |
| --- | --- |
| `default` | 매핑 요청이 없을 때의 구성 = 최소 요건 매핑 |
| 필수 항목 (`modes` 또는 `required`) | 제어나 채널 해석에 꼭 필요한 객체. 순서 = 매핑 순서 |
| `extras` | 요청할 때만 넣는 추가 항목 묶음. 순서 = 우선순위 |
| `cyclic` (`rx`/`tx`) | PDO에 넣는 객체 |
| `on_event` | PDO에 넣지 않고 필요할 때 SDO로 읽는 객체 (에러 기록 등) |

## 파일

| 파일 | 대상 | 선택 조건 |
| --- | --- | --- |
| `infrastructure.yaml` | 커플러, 확장 모듈 | PDO 없음 |
| `non_coe_io.yaml` | CoE 미지원 I/O | CoE 미지원, PDO 있음 |
| `cia402.yaml` | 드라이브 | 0x1000 하위 16비트 = 402 |
| `mdp5001.yaml` | MDP 모듈형 I/O | 0x1000 하위 16비트 = 5001 |
| `cia401.yaml` | 범용 I/O | 0x1000 하위 16비트 = 401 |
| `models/*.yaml` | 특정 모델 예외, 승인된 LLM 결과 | Vendor ID + 0x1008 (없으면 SII 주문번호, 이름이 없으면 product code). `base_rule: null`이면 프로파일 rule이 없는 장치용 |
| `llm_vocabulary.yaml` | LLM 출력 선택지 (형태, 역할, 방향 규칙) | - |

## 매핑 요청 형식

```yaml
mode: csv                         # CiA402만 해당. 없으면 default.mode
extras: [state_monitor, error_detect]
```

요청이 없으면 `default`를 쓴다. 자연어 요청은 LLM이 이 형식으로 바꾼다 (선택지 = rule의 mode와 extras 이름).

## 매핑 결정 순서

1. 요청 확인. 없으면 `default`
2. (CiA402) 0x6502에서 요청 모드 지원 확인. 미지원이면 실패
3. 필수 항목이 장치에 모두 있는지 확인. 없으면 실패 → LLM
4. 요청한 extras 중 장치에 있는 객체만 선택. 없는 객체는 보고
5. CoE Details로 매핑 방법 결정
   - bit 3 = 1: 필수 → extras 순으로 새 매핑
   - bit 2만 1: 필요한 객체를 포함하는 고정 PDO 세트 선택
   - 둘 다 0 / CoE 미지원: 현재(또는 SII) 매핑 그대로
6. 출력: 매핑 계획 + on_event SDO 조회 목록 + 빠진 항목 보고

## 확인이 필요한 값

- `cia402.yaml`의 CST 필수 항목 중 0x6064, 0x606C는 RAON-RT 토크 제어 피드백 기준으로 넣었다.
- `cia401.yaml`의 객체 배치는 CANopen 기준이며 EtherCAT 실장비로 검증하지 않았다.
- `llm_vocabulary.yaml`의 형태·역할 이름은 RAON-RT cfg 항목에 맞춰 확정해야 한다.

## rule 엔진

`tools/ecat_rules/rule_engine.py`가 이 형식을 읽고 위 순서대로 매핑을 결정한다 (사용법은 `tools/ecat_rules/README.md`).
이전 엔진은 선택 객체가 장치에 있으면 자동으로 넣었지만, v2는 요청한 extras만 넣는다.
