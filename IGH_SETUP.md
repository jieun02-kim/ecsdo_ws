# IgH EtherCAT Master 설치 (libecsdo 사용 전 준비)

libecsdo는 IgH EtherCAT Master의 커널 모듈(`ec_master`)에 ioctl을 직접 호출한다.
그래서 **IgH 커널 모듈이 로드되어 있어야 하고**, 빌드할 때는 **`./configure`를 마친 IgH 소스 트리**가 필요하다
(`master/ioctl.h`, 최상위 `config.h`).

| 항목 | 개발·검증 환경 |
|---|---|
| IgH EtherCAT Master | **1.6.13** (`stable-1.6` 브랜치, ioctl 버전 매직 37) |
| OS / 커널 | Ubuntu, 6.8.0-generic |
| 이더넷 드라이버 | `generic` (모든 NIC에서 동작) |

> 다른 IgH 버전도 빌드는 될 수 있지만 ioctl 버전 매직이 다르면 `ecsdo_open()`이 `ECSDO_ERR_VERSION`으로 실패한다.
> libecsdo는 **로드된 모듈과 같은 소스 트리**로 빌드해야 한다.

아래 명령에서 `<NIC>`는 EtherCAT 장치를 연결한 랜 포트 이름이다 (예: `enp5s0`, `ip link`로 확인).

---

## 1. 필요한 패키지

```sh
sudo apt update
sudo apt install -y build-essential git autoconf automake libtool pkg-config \
                    cmake linux-headers-$(uname -r)
# 선택: valgrind (libecsdo 메모리 검사)
```

## 2. IgH 소스 받기

```sh
git clone https://gitlab.com/etherlab.org/ethercat.git
cd ethercat
git checkout 1.6.13          # 검증한 버전 (태그)
./bootstrap                  # configure 스크립트 생성
```

## 3. configure / 빌드

```sh
./configure --prefix=/usr/local --sysconfdir=/etc \
            --enable-generic \
            --disable-8139too --disable-e100 --disable-e1000 --disable-e1000e \
            --disable-r8169 --disable-igb --disable-igc --disable-ccat --disable-stmmac \
            --with-linux-dir=/usr/src/linux-headers-$(uname -r)
make -j$(nproc)
make modules -j$(nproc)
```

- `--enable-generic`: 일반 리눅스 네트워크 드라이버 위에서 동작하는 `ec_generic`. NIC 종류와 관계없이 쓸 수 있다.
  나머지 전용 드라이버는 끈다 (커널 버전에 맞는 패치가 없으면 빌드가 실패할 수 있음).
- `--sysconfdir=/etc`: 설정 파일이 `/etc/ethercat.conf`에 설치된다.
- 이 소스 트리 경로가 나중에 libecsdo의 `-DIGH_SOURCE_DIR`이 된다. **지우지 말 것.**

## 4. 설치

```sh
sudo make install modules_install
sudo depmod -a

# libethercat (/usr/local/lib) 경로 등록, ethercat 명령을 PATH에
echo "/usr/local/lib" | sudo tee /etc/ld.so.conf.d/ethercat.conf
sudo ldconfig
sudo ln -sf /usr/local/bin/ethercat /usr/bin/ethercat
```

## 5. 마스터 설정 (`/etc/ethercat.conf`)

EtherCAT용 NIC의 MAC 주소와 드라이버를 지정한다.

```sh
MAC=$(cat /sys/class/net/<NIC>/address)
sudo sed -i "s/^MASTER0_DEVICE=.*/MASTER0_DEVICE=\"$MAC\"/; \
             s/^DEVICE_MODULES=.*/DEVICE_MODULES=\"generic\"/" /etc/ethercat.conf
grep -E '^(MASTER0_DEVICE|DEVICE_MODULES)=' /etc/ethercat.conf
```

> **주의:** `sudo make install`을 다시 실행하면 `/etc/ethercat.conf`가 빈 기본값으로 덮어써진다.
> 재설치할 때마다 이 단계를 다시 할 것.

## 6. `/dev/EtherCAT0` 권한 (udev)

libecsdo의 사전 읽기는 읽기 권한, **SDO 값 upload는 읽기+쓰기 권한**이 필요하다.
`ethercat` 그룹에만 권한을 주는 것을 권장한다:

```sh
sudo groupadd --system ethercat
sudo usermod -aG ethercat $USER          # 다시 로그인해야 적용
echo 'KERNEL=="EtherCAT[0-9]*", GROUP="ethercat", MODE="0660"' \
    | sudo tee /etc/udev/rules.d/99-EtherCAT.rules
sudo udevadm control --reload-rules
```

(개발 PC에서 간단히 쓰려면 `MODE="0666"`으로 모든 사용자에게 열 수 있지만, 그러면 누구나 SDO 쓰기·상태 변경을 할 수 있다.)

## 7. 서비스 시작

```sh
sudo systemctl daemon-reload
sudo systemctl enable ethercat           # 부팅 시 자동 시작
sudo systemctl restart ethercat
systemctl --no-pager status ethercat
```

## 8. 확인

```sh
lsmod | grep ec_                 # ec_master, ec_generic
ls -l /dev/EtherCAT0
ethercat master                  # Main: <MAC> (attached), Link: UP
ethercat slaves                  # 연결된 슬레이브 목록
```

- `Link: DOWN`이면 케이블, 장치 전원, `MASTER0_DEVICE`의 MAC을 확인한다.
- `Slaves: 0`이면서 링크가 UP이면 첫 번째 슬레이브의 IN 포트에 연결했는지 확인한다.
- 모듈이 로드되지 않고 `Key was rejected by service`가 나오면 Secure Boot 때문이다
  (BIOS에서 끄거나 모듈에 서명해야 한다).

## 9. libecsdo 빌드

```sh
git clone https://github.com/jieun02-kim/ecsdo_ws.git
cd ecsdo_ws
cmake -S libecsdo -B build -DIGH_SOURCE_DIR=<IgH 소스 경로> -DCMAKE_INSTALL_PREFIX=$PWD/install
cmake --build build
(cd build && ctest)                      # 장치 없이 단위 테스트
cmake --install build

./install/bin/ecsdo_cli -p 0 sdos         # = ethercat sdos -p 0
```

자세한 사용법은 [`libecsdo/README.md`](libecsdo/README.md).

---

## 커널이 업데이트된 경우

Ubuntu가 커널을 자동으로 올리면 새 커널용 `ec_master`가 없어서 서비스가 시작되지 않는다
(`ethercat master`가 실패하고 `/dev/EtherCAT0`이 없다). IgH 소스 트리에서 다시 빌드한다:

```sh
sudo apt install -y linux-headers-$(uname -r)
cd <IgH 소스 경로>
./configure <3단계와 같은 옵션, --with-linux-dir=/usr/src/linux-headers-$(uname -r)>
make -j$(nproc) && make modules -j$(nproc)
sudo make install modules_install
sudo depmod -a
# make install이 /etc/ethercat.conf를 덮어쓰므로 5단계를 다시 한다
sudo systemctl restart ethercat
```

IgH 버전을 바꾸지 않았다면 libecsdo는 다시 빌드하지 않아도 된다 (ioctl 버전이 같음).
IgH 버전을 바꿨다면 libecsdo도 다시 빌드한다.
