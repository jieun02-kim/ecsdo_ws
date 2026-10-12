# device_probe 실행 순서

작업 공간: `~/ecsdo_ws`

## 1. 마스터 기동

```sh
sudo systemctl restart ethercat
ethercat slaves
```

슬레이브가 `PREOP`로 보이면 준비된 것이다.
안 보이면 케이블, 전원, `/etc/ethercat.conf`의 MAC 주소를 확인한다 (`IGH_SETUP.md` 참고).

## 2. 빌드

처음 한 번:

```sh
cd ~/ecsdo_ws
cmake -S libecsdo -B libecsdo/build -DIGH_SOURCE_DIR=<IgH 소스 경로>
cmake --build libecsdo/build
```

코드를 바꾼 뒤:

```sh
cmake --build libecsdo/build
```

## 3. 실행

```sh
sudo ./libecsdo/build/device_probe
```

- `sudo`가 필요하다. SDO upload에 `/dev/EtherCAT0` 쓰기 권한이 있어야 한다.
- 결과는 현재 디렉터리의 `dump/slave_<pos>.json`에 생긴다.

| 옵션 | 기본값 | 뜻 |
|---|---|---|
| `-o DIR` | `dump` | 출력 디렉터리 |
| `-m MASTER` | 0 | 마스터 번호 |
| `-t MS` | 30000 | 사전 대기 최대 시간 |
| `-v FILE` | 소스의 `vendor_ids.json` | vendor_id → 이름 표 |

## 4. 결과 확인

```sh
ls dump/
cat dump/slave_0.json
```

화면에는 슬레이브마다 판별 과정이 출력된다.

```
[slave 0] "..." vendor 0x... product 0x...
  1. process data (SII): yes ...
  2. CoE: yes
  3. SDO info (SII General): yes
  4. 0x1000 = 0x00020192, profile 402 -> cia402
  => cia402, 0 error(s) -> dump/slave_0.json
```

## 장비 없이 확인 (시뮬레이션)

```sh
ctest --test-dir libecsdo/build -R probe_sim
cat libecsdo/build/probe_sim_dump/slave_0.json
```

가짜 장치로 만든 결과이며, 실제 장비 값이 아니다.

분기 로직과 JSON 필드 설명은 `README.md`에 있다.
