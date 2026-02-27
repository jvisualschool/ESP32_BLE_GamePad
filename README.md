# 📟 ESP32-S3 3.5" QSPI Smart Display + BLE Gamepad

**Sunton ESP32-S3 3.5인치 QSPI 디스플레이 보드**에서 구동되는 **LVGL 기반 시스템 인터페이스**입니다.  
AXS15231B 디스플레이 드라이버와 BLE HID 게임패드(ShanWan Q36)를 통합하여, 터치/게임패드 양쪽으로 테마를 전환할 수 있는 데모 프로젝트입니다.

![Project Screenshot](screenshot/screenshot1_slim.webp)

| Theme: DARK | Theme: LIGHT | Theme: LOW_POWER |
| :---: | :---: | :---: |
| ![Dark Mode](screenshot/screenshot1_slim.webp) | ![Light Mode](screenshot/screenshot3_slim.webp) | ![Low Power](screenshot/screenshot4_slim.webp) |

---

## 📑 목차

1. [하드웨어 (H/W)](#-하드웨어-hw)
2. [소프트웨어 (S/W)](#-소프트웨어-sw)
3. [핀 맵 (Pin Map)](#-핀-맵-pin-map)
4. [API 레퍼런스](#-api-레퍼런스)
5. [BLE 게임패드 프로토콜](#-ble-게임패드-프로토콜)
6. [빌드 & 업로드](#-빌드--업로드)
7. [주요 이슈 & 해결 방법](#-주요-이슈--해결-방법)
8. [파일 구조](#-파일-구조)
9. [업데이트 기록](#-업데이트-기록)
10. [향후 계획](#-향후-계획)

---

## 🔩 하드웨어 (H/W)

### 메인 보드

| 항목 | 사양 |
|---|---|
| **MCU** | ESP32-S3 (QFN56, revision v0.2) |
| **CPU** | Dual-Core Xtensa LX7 @ 240MHz + LP Core |
| **Flash** | 16MB |
| **PSRAM** | 8MB (OPI, Octal SPI) |
| **무선** | Wi-Fi 802.11 b/g/n + Bluetooth 5.0 (LE) |
| **USB** | USB-Serial/JTAG (CDC on Boot) |
| **MAC** | `3C:0F:02:D0:25:14` |

### 디스플레이

| 항목 | 사양 |
|---|---|
| **패널** | 3.5인치 TFT LCD |
| **IC** | AXS15231B |
| **인터페이스** | QSPI (Quad SPI) |
| **해상도** | 320 × 480 (가로 × 세로) |
| **색상 포맷** | RGB565 (16-bit, Big Endian) |
| **백라이트** | PWM 제어 (LEDC, 10-bit 해상도, 5kHz) |
| **터치** | I2C 방식 정전식 터치 (AXS15231B 내장 터치 IC) |
| **Tear Effect** | TE 핀 기반 VSync 동기화 지원 |

### BLE 게임패드

| 항목 | 사양 |
|---|---|
| **모델** | ShanWan Q36 |
| **프로토콜** | Bluetooth Low Energy (BLE) HID |
| **HID Service** | UUID `0x1812` |
| **Report Characteristic** | UUID `0x2A4D` (17바이트 HID Report) |
| **연결 방식** | ESP32-S3가 BLE Central로 동작, 게임패드가 Peripheral |

---

## 💻 소프트웨어 (S/W)

### 개발 환경

| 항목 | 버전 / 도구 |
|---|---|
| **프레임워크** | Arduino (ESP32 Arduino Core) |
| **빌드 도구** | Arduino CLI |
| **보드 패키지** | `esp32:esp32` v3.3.6 |
| **FQBN** | `esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi` |
| **파티션** | app: 3MB / FAT: 9MB (총 16MB) |

### 라이브러리 의존성

| 라이브러리 | 버전 | 용도 |
|---|---|---|
| **LVGL** | v8.3.9 | GUI 렌더링 엔진 |
| **BLE** | v3.3.6 (ESP32 내장) | BLE Central 통신 |

### 소프트웨어 아키텍처

```
┌──────────────────────────────────────────────────────────┐
│                    Arduino loop()                        │
│  ┌─────────────────────┐  ┌───────────────────────────┐  │
│  │   processGamepad()  │  │  Backlight Animation      │  │
│  │  - BLE scan/connect │  │  - 10ms 간격 1%씩 변화     │  │
│  │  - auto reconnect   │  │  - target ↔ current 보간  │  │
│  └─────────┬───────────┘  └───────────────────────────┘  │
│            │                                             │
│            ▼                                             │
│  ┌─────────────────────┐                                 │
│  │  notifyCallback()   │◄── BLE HID Report (17 bytes)    │
│  │  - idle 필터링       │                                 │
│  │  - 500ms 디바운스    │                                 │
│  └─────────┬───────────┘                                 │
│            │                                             │
│            ▼                                             │
│  ┌─────────────────────┐  ┌───────────────────────────┐  │
│  │ toggle_next_theme() │◄─┤  screen_event_cb()        │  │
│  │  DARK→LIGHT→LOW_PWR │  │  (터치 클릭 이벤트)        │  │
│  └─────────┬───────────┘  └───────────────────────────┘  │
│            │                                             │
│            ▼                                             │
│  ┌─────────────────────┐                                 │
│  │   update_theme()    │                                 │
│  │  - 배경/텍스트 색상   │                                │
│  │  - 백라이트 목표치     │                                │
│  │  - LVGL Transition   │                                │
│  └─────────────────────┘                                 │
└──────────────────────────────────────────────────────────┘

┌─────────────── BSP Layer ────────────────┐
│  esp_bsp.c / esp_bsp.h                   │
│  ├── bsp_display_start_with_config()     │
│  ├── bsp_display_brightness_set()        │
│  ├── bsp_display_lock/unlock()           │
│  └── bsp_touch_new()                     │
├──────────────────────────────────────────┤
│  esp_lcd_axs15231b.c / .h                │
│  ├── QSPI 패널 초기화 (67개 init cmd)     │
│  ├── esp_lcd_new_panel_axs15231b()       │
│  └── Tear Effect VSync 동기화            │
├──────────────────────────────────────────┤
│  lv_port.c / lv_port.h                   │
│  ├── lvgl_port_init()                    │
│  ├── lvgl_port_add_disp()                │
│  ├── lvgl_port_add_touch()               │
│  └── LVGL Task (FreeRTOS)                │
└──────────────────────────────────────────┘
```

---

## 📌 핀 맵 (Pin Map)

### QSPI 디스플레이

| 핀 | GPIO | 설명 |
|---|---|---|
| CS | GPIO_45 | Chip Select |
| PCLK | GPIO_47 | Pixel Clock (SPI Clock) |
| DATA0 | GPIO_21 | QSPI Data 0 (MOSI) |
| DATA1 | GPIO_48 | QSPI Data 1 |
| DATA2 | GPIO_40 | QSPI Data 2 |
| DATA3 | GPIO_39 | QSPI Data 3 |
| RST | NC | Reset (미사용, 소프트웨어 리셋) |
| DC | GPIO_8 | Data/Command 선택 |
| TE | GPIO_38 | Tear Effect (VSync 동기화) |
| BL | GPIO_1 | 백라이트 PWM |

### I2C 터치패널

| 핀 | GPIO | 설명 |
|---|---|---|
| SCL | GPIO_8 | I2C Clock (DC 핀과 공유) |
| SDA | GPIO_4 | I2C Data |
| RST | -1 | 미사용 |
| INT | -1 | 미사용 |

### 기타 설정

| 항목 | 값 |
|---|---|
| SPI Host | SPI2_HOST |
| I2C Bus | I2C_NUM_0 |
| I2C Speed | 400kHz |
| LEDC Channel | 1 |
| LEDC Timer | 1 (5kHz, 10-bit) |

---

## 📚 API 레퍼런스

### BSP (Board Support Package) — `esp_bsp.h`

| 함수 | 반환 | 설명 |
|---|---|---|
| `bsp_display_start_with_config(cfg)` | `lv_disp_t*` | QSPI 디스플레이 + LVGL + 터치 초기화 (All-in-One) |
| `bsp_display_brightness_set(percent)` | `esp_err_t` | 백라이트 밝기 설정 (0~100%) |
| `bsp_display_backlight_on()` | `esp_err_t` | 백라이트 100% 켜기 |
| `bsp_display_backlight_off()` | `esp_err_t` | 백라이트 끄기 |
| `bsp_display_lock(timeout_ms)` | `bool` | LVGL 뮤텍스 획득 (UI 업데이트 전 필수) |
| `bsp_display_unlock()` | `void` | LVGL 뮤텍스 해제 |
| `bsp_display_get_input_dev()` | `lv_indev_t*` | 터치 입력 디바이스 포인터 |
| `bsp_i2c_init()` | `esp_err_t` | I2C 버스 초기화 (터치 통신용) |

### 디스플레이 드라이버 — `display.h`

| 함수 | 반환 | 설명 |
|---|---|---|
| `bsp_display_new(config, panel, io)` | `esp_err_t` | 저수준 LCD 패널 생성 (QSPI + TE 동기화) |

### LVGL 포트 — `lv_port.h`

| 함수 | 반환 | 설명 |
|---|---|---|
| `lvgl_port_init(cfg)` | `esp_err_t` | LVGL 엔진 초기화 + FreeRTOS 태스크 생성 |
| `lvgl_port_add_disp(disp_cfg)` | `lv_disp_t*` | 디스플레이를 LVGL에 등록 |
| `lvgl_port_add_touch(touch_cfg)` | `lv_indev_t*` | 터치 입력을 LVGL에 등록 |
| `lvgl_port_lock(timeout_ms)` | `bool` | LVGL 뮤텍스 잠금 |
| `lvgl_port_unlock()` | `void` | LVGL 뮤텍스 해제 |

### 애플리케이션 레벨 — `ESP32_HelloWorld_AG.ino`

| 함수 | 설명 |
|---|---|
| `setup()` | 디스플레이·BLE·UI 전체 초기화 |
| `loop()` | 게임패드 폴링 + 백라이트 애니메이션 (10ms 주기) |
| `toggle_next_theme()` | DARK → LIGHT → LOW_POWER 테마 순환 |
| `update_theme()` | 현재 테마에 따라 UI 색상·백라이트 적용 |
| `connectToServer()` | BLE 게임패드 연결 + HID 서비스 구독 |
| `processGamepad()` | 연결 상태 관리 + 자동 재스캔 |
| `notifyCallback()` | HID Report 수신 콜백 (입력 처리) |
| `enableCCCD(pChar)` | CCCD(0x2902) Descriptor 직접 활성화 |
| `startBLEScan()` | BLE 스캔 시작 (10초간) |

---

## 🎮 BLE 게임패드 프로토콜

### 연결 흐름

```
[ESP32-S3 Central]                    [ShanWan Q36 Peripheral]
       |                                       |
       |── BLE Scan (Active, 10s) ────────────>|
       |                                       |
       |<── Advertisement ("ShanWan Q36") ─────|
       |                                       |
       |── Set Security (ESP_LE_AUTH_BOND) ───>|
       |── Connect ───────────────────────────>|
       |<── onConnect callback ────────────────|
       |                                       |
       |── delay(1000ms) ─ 안정화 대기          |
       |                                       |
       |── Get Service (0x1812 HID) ──────────>|
       |<── Service + Characteristics ─────────|
       |                                       |
       |── registerForNotify (0x2A4D) ────────>|
       |── Write CCCD (0x2902 = 0x0001) ──────>|
       |                                       |
       |<── HID Report (17 bytes, Notify) ─────| ← 버튼/조이스틱 입력
       |<── HID Report (17 bytes, Notify) ─────|
       |         ...                           |
```

### HID Service 구조

| UUID | 이름 | 속성 | 설명 |
|---|---|---|---|
| `0x2A4A` | HID Information | Read | HID 디바이스 정보 |
| `0x2A4B` | Report Map | Read | HID Report Descriptor |
| `0x2A4C` | HID Control Point | — | HID 제어 포인트 |
| **`0x2A4D`** | **Report** | **Notify, Read, Write** | **입력 데이터 (17바이트)** |
| `0x2A4E` | Protocol Mode | Read | Boot/Report 모드 |

### HID Report 데이터 (17바이트)

```
Byte[0]:  Report ID (02=idle position, 03=active input)
Byte[1]:  Joystick X (좌=0x00, 중립≈0x80~0xCC, 우=0xFF)
Byte[2]:  Joystick Y (상=0x00, 중립≈0x80, 하=0xFF)
Byte[3]:  R-Stick / 기타 축
Byte[4-15]: 버튼 상태 (대부분 0x00)
Byte[16]: 항상 0x01 (패딩 또는 배터리)
```

### idle 필터링 로직

조이스틱 idle 상태에서도 `0x7F`, `0x80` 근방의 값이 계속 전송되므로, `0x00`, `0x7F`, `0x80` 이외의 값이 포함된 Report만 실제 입력으로 간주합니다.

---

## 🛠️ 빌드 & 업로드

### 전제 조건

- **Arduino CLI** 설치 완료
- **보드 매니저:** `esp32:esp32` v3.3.6
- **라이브러리:** `lvgl` v8.3.9
- **포트:** `/dev/cu.usbmodem101` (시스템에 따라 변동)

### 컴파일

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi \
  .
```

### 업로드

```bash
arduino-cli upload \
  -p /dev/cu.usbmodem101 \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi \
  .
```

> ⚠️ 업로드 실패 시: 보드의 **Boot + RST** 버튼 콤보를 사용하세요.

### 시리얼 모니터

```bash
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

### ⚡ 중요: 폴더명과 .ino 파일명 일치 필수

Arduino CLI는 **폴더명과 `.ino` 파일명이 동일**해야 합니다.  
이 프로젝트의 경우: 폴더 `ESP32_HelloWorld_AG/` → 파일 `ESP32_HelloWorld_AG.ino`

---

## 🐛 주요 이슈 & 해결 방법

### Issue #1: 디스플레이가 왼쪽에 세로줄 한 줄만 표시됨

**증상:** 화면 좌측에 깨진 세로줄 1~2줄만 보이고 나머지는 검은 화면  
**원인:** QSPI 통신 설정 오류 또는 잘못된 디스플레이 드라이버 사용  
**해결:**
- 제조사 전용 **AXS15231B QSPI 드라이버** 적용 (Espressif BSP 기반)
- 67개의 LCD 초기화 커맨드 시퀀스 정확히 적용 (`lcd_init_cmds[]`)
- `use_qspi_interface = 1` 플래그 설정 확인
- QSPI 4-line 모드 핀(DATA0~DATA3) 올바른 매핑 확인

### Issue #2: BLE 게임패드 연결 실패 (반짝이며 대기 상태 지속)

**증상:** 게임패드 LED가 계속 반짝이며 페어링 대기, 연결 안 됨  
**원인:** BLE Security 설정이 `connect()` 이후에 되고 있어 HID 기기가 암호화를 확인할 수 없음  
**해결:**
```cpp
// ❌ 잘못된 순서 (v1.0)
pClient->connect(targetDevice);          // 먼저 연결
BLESecurity *pSecurity = new BLESecurity();  // 그 다음 보안 설정 → 너무 늦음

// ✅ 올바른 순서 (v1.1)
BLESecurity *pSecurity = new BLESecurity();  // 먼저 보안 설정
pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
pClient->connect(targetDevice);              // 그 다음 연결
```

### Issue #3: HID 서비스는 찾았지만 버튼 입력 데이터가 안 들어옴

**증상:** `Subscribed to 1 HID report(s)` 로그는 나오지만 `notifyCallback` 호출 안 됨  
**원인:** `registerForNotify()`만으로는 일부 HID 기기에서 CCCD가 활성화되지 않음  
**해결:** CCCD(Client Characteristic Configuration Descriptor, UUID 0x2902)에 **직접 `0x0001` 쓰기:**
```cpp
BLERemoteDescriptor* pDesc = pChar->getDescriptor(BLEUUID((uint16_t)0x2902));
uint8_t notifyOn[] = {0x01, 0x00};
pDesc->writeValue(notifyOn, 2, true);
```

### Issue #4: `ESP_LE_AUTH_REQ_SC_BOND` 인증 모드에서 연결 불안정

**증상:** Secure Connections (SC) 모드에서 게임패드가 간헐적 연결 끊김  
**원인:** 저가형 BLE 게임패드는 SC(Secure Connections)를 미지원하는 경우가 많음  
**해결:** `ESP_LE_AUTH_BOND` (단순 본딩)으로 변경하여 호환성 확보:
```cpp
pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);  // SC 없이 단순 본딩
```

### Issue #5: 연결 직후 서비스 탐색 실패

**증상:** `connect()` 성공 직후 `getService(0x1812)` 호출 시 `nullptr` 반환  
**원인:** BLE 연결 직후에는 서비스 인덱스가 아직 준비되지 않음  
**해결:** 연결 후 **1초 딜레이** 추가:
```cpp
pClient->connect(targetDevice);
connected = true;
delay(1000);  // 서비스 탐색 안정화
BLERemoteService* pSvc = pClient->getService(BLEUUID((uint16_t)0x1812));
```

### Issue #6: 게임패드 재연결 안 됨 (전원 껐다 켰을 때)

**증상:** 게임패드 전원 OFF/ON 후 ESP32가 재연결 시도하지 않음  
**원인:** `onDisconnect` 콜백 내에서 직접 `scan->start()`를 호출하면 BLE 스택 충돌 가능  
**해결:** `loop()`에서 5초 간격으로 **비동기 재스캔:**
```cpp
void processGamepad() {
    if (!connected && !scanning) {
        if (millis() - lastReconnectAttempt > 5000) {
            startBLEScan();  // loop 컨텍스트에서 안전하게 재스캔
        }
    }
}
```

### Issue #7: 폴더명과 .ino 파일명 불일치 빌드 에러

**증상:** `main file missing from sketch` 오류  
**원인:** Arduino CLI는 폴더명과 `.ino` 파일명이 일치해야 함  
**해결:** 파일명을 폴더명과 일치시킴 (`ESP32_HelloWorld.ino` → `ESP32_HelloWorld_AG.ino`)

> ⚠️ **심볼릭 링크(symlink)로 해결하면 안 됨!** Arduino는 `.ino` 파일을 모두 병합 컴파일하므로 원본과 링크 파일이 이중 컴파일되어 `redefinition` 오류 발생.

---

## 📂 파일 구조

```
ESP32_HelloWorld_AG/
├── ESP32_HelloWorld_AG.ino    # 메인 애플리케이션 (UI + BLE 게임패드)
├── display.h                  # 해상도, 색상 포맷, 핀 #define
├── esp_bsp.h                  # BSP API 헤더 (디스플레이·터치·I2C)
├── esp_bsp.c                  # BSP 구현체 (QSPI 초기화, 백라이트, 터치)
├── esp_lcd_axs15231b.h        # AXS15231B 드라이버 헤더
├── esp_lcd_axs15231b.c        # AXS15231B 드라이버 (QSPI 통신, 초기화 시퀀스)
├── esp_lcd_touch.h            # LCD 터치 인터페이스 헤더
├── esp_lcd_touch.c            # LCD 터치 구현체
├── lv_port.h                  # LVGL 포팅 레이어 헤더
├── lv_port.c                  # LVGL 포팅 구현체 (태스크, 버퍼, flush)
├── lv_conf.h                  # LVGL 설정 (폰트, 색상, 기능 on/off)
├── bsp_err_check.h            # 에러 체크 매크로
├── platformio.ini             # PlatformIO 설정 (참고용)
├── index.html                 # 에뮬레이터 HTML (테마 전환 시뮬레이션)
├── screenshot/                # 스크린샷 이미지
│   ├── screenshot1_slim.webp  # DARK 테마
│   ├── screenshot3_slim.webp  # LIGHT 테마
│   └── screenshot4_slim.webp  # LOW_POWER 테마
└── README.md                  # 이 문서
```

---

## 🆙 업데이트 기록 (Changelog)

### [v1.1] - 2026-02-27

- **BLE 게임패드 연결 안정화**
  - Security를 `connect()` 전에 선행 설정 (`ESP_LE_AUTH_BOND`)
  - CCCD(0x2902) Descriptor **직접 활성화** (이중 보장)
  - 연결 후 1초 안정화 딜레이 추가
  - `onDisconnect` 시 자동 재스캔 (5초 간격, loop 컨텍스트)
- **idle 입력 필터링**: `0x00`, `0x7F`, `0x80` 데이터만 있는 Report 무시
- **디버그 로그 체계화**: `[BLE]`, `[GP]` 태그 기반 구조화 로그
- **파일명 정리**: `ESP32_HelloWorld.ino` → `ESP32_HelloWorld_AG.ino` (폴더명 일치)

### [v1.0] - 2026-02-27

- **디스플레이 드라이버 복구**: AXS15231B QSPI 드라이버 정상화
- **BLE 게임패드 발견**: ShanWan Q36 스캔 및 기본 연결 구현
- **LVGL UI**: HELLO WORLD 테마 전환 UI (DARK/LIGHT/LOW_POWER)
- **터치 입력**: 화면 터치 시 테마 순환
- **백라이트 애니메이션**: 10ms 간격 1%씩 부드러운 밝기 전환

### [v1.5-legacy] - 2026-02-08

- 날짜 및 빌드 환경 최적화
- 저전력 모드(LOW_POWER) 추가
- LVGL Transition 기능 활용한 색상 변화 애니메이션

---

## 🚀 향후 계획 (Next Steps)

1. **게임패드 버튼 매핑**: 각 버튼별 기능 분리 (A=테마, B=밝기, 조이스틱=메뉴 이동)
2. **터치 기능 확장**: 스와이프 제스처, 길게 누르기 등 고급 터치 이벤트
3. **PSRAM 활용**: 8MB PSRAM으로 이미지 에셋·버퍼 확대
4. **위젯 확장**: 실시간 시계, 센서 데이터 모니터링, 배터리 잔량 표시
5. **Wi-Fi 연동**: NTP 시간 동기화, MQTT 센서 데이터 수신

---

**Developer:** Jinho Jung  
**Last Updated:** 2026-02-27
