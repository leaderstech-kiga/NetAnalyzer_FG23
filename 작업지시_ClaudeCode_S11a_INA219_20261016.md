# 작업지시 (Claude Code) — **S11a** INA219 I²C 읽기 (전류·전압 실측)

> **프로젝트** `NetAnalyzer_FG23_V01` · **현재** `FS_N=6` `FS_POWER=1` `FS_WDOG=1`
> **작성** 2026-10-16 · **범위** **I²C 로 INA219 를 읽어 배너/로그에 찍는 것까지.**
> `TYPE='B'` 프레임 송신은 **S11b** 로 분리한다. (한 번에 한 변수)
> **근거** `../Network_Analyzer/INA219_전류감시_회로설계.md` · `FG23_UART_프로토콜_v2_확장안_20261014.md §3.4`

---

## 0. 작업 원칙 (필수)

0. **★판단 기준(최상위)** = `JUDGMENT_V1.1.md`. **"내 문제로 인식"**, **안전 절대선**(소방 경보·연동
   방해 금지 — 스니퍼는 수동 관측 우선), 규격 근거, **추측보다 실측**, 단계적·병행+fallback.
   ★응답은 **"등불"** — 답+리스크 함께 제시, 원칙과 어긋나면 즉시 되묻는다.
1. **데이터 기반 — 추측 금지.** 근거 링크를 주석에 남긴다. `silicon-labs-docs` MCP 사용.
2. **변경 시 검증 루프**: 빌드 → **실측** → 결과 → 다음. **한 번에 한 변수.**
3. **코드 변경 시 원본을 주석으로 남긴다.**
4. **★`@소장:` 태그 최우선.** 작업 전 `grep -rn "@소장"` 전수 반영 후 태그 삭제.
5. **스텝 가드 + known-good 앵커.** 통과 시 `_known_good/` 보관 + `step_config.h` 이력 1줄.

---

## 1. 왜 이걸 먼저 하는가

**전원 시퀀스(§0.2)의 미결이 전부 여기에 걸려 있다.**

`FG23_UART_프로토콜_v2_확장안 §0.2` 가 저전압 차단 대기시간을 **"값"이 아니라 "창(window)"** 으로
규정했는데, 그 창의 **상한**을 재려면 INA219 가 필요하다:

```
하한 = Android 정상 종료 완료 시간            (짧으면 eMMC 손상)
상한 = BAT_CRITICAL 전압에서 0.7A 부하로
       8.4V(팩 BMS 차단)까지 걸리는 시간      ← ★INA219 없이는 못 잰다
```

→ **`BAT_CRITICAL` 임계값(현 잠정 9.0V)이 이 측정으로 확정된다.**
덤으로 `회로검토 2V0` 의 잔존 지적(`R33` 3216 패키지 발열)도 같이 닫힌다.

---

## 2. 회로 사실 (2V0, 도면 확대 판독 완료)

```
Bat+ ──[R33 0.01Ω]── V_SYS ──→ 벅 VIN
        │              │
      VIN−(2)       VIN+(1)        C15 0.1µF = VIN+/VIN− 차동 필터
        │              │           R31/R32 = 10Ω 입력 직렬
        └── INA219AID ─┘
             GND(3) ─ GND
             VS(4)  ─ +3.3V
             SCL(5) ─ PC02 ──[R57 10K]── +3.3V
             SDA(6) ─ PC03 ──[R58 10K]── +3.3V
             A0(7),A1(8) ─ R59/R65 10K ─ GND   → slave 0x40
```

| 사실 | 값 |
|---|---|
| I²C 핀 | **SCL = PC02 / SDA = PC03** (`board_cfg.h` 에 기록됨) |
| slave 주소 | **0x40** (A0/A1 풀다운) |
| **외부 풀업** | **R57/R58 = 10 kΩ** → ★**내부 풀업 켜지 마라** |
| 션트 | `R33` **0.01 Ω** (패키지 3216 — 발열 확인 대상, §5-5) |
| **극성** | `VIN+` = V_SYS 측 / `VIN−` = Bat+ 측 → **`+`=충전 / `−`=방전** |

**★극성은 이미 실측으로 확인됐다** (2026-10-14, 어댑터 연결 상태):
`Vshunt = +5.9 mV` → `590 mA`, 부호 `+` = 충전. 설계 의도대로 동작.
→ **펌웨어에서 부호를 뒤집지 마라.**

---

## 3. ★설계 판단 — I²C 를 어떻게 구현할 것인가

**확정 (2026-10-16, 소장 지시): 하드웨어 I²C0 를 쓴다. `.slcp` 는 소장이 직접 수정한다.**
Claude Code 는 `.slcp` · `.pintool` · `config/` 를 **건드리지 않는다.**

### 3.1 하드웨어 I²C 가 가능하다고 판단한 근거 (검증 완료)

1. **emlib 은 이미 빌드에 있다.** `subdir.mk` 컴파일 목록:
   `em_cmu.c` · `em_gpio.c` · `em_eusart.c` · `em_burtc.c` · `em_ldma.c` ·
   `em_msc.c` · `em_prs.c` · `em_system.c` · `em_timer.c`.
   빠진 것은 **`em_i2c.c` 하나뿐**이다. 새 레이어를 들이는 게 아니라 같은 레이어에 파일 하나를 더하는 것.
2. **PC02/PC03 에 I²C0 라우팅이 된다.** `efr32fg23_gpio.h`:
   `GPIO_I2CROUTE_TypeDef I2CROUTE[2U]` (I2C0·I2C1 각각),
   `_GPIO_I2C_SCLROUTE_PORT_MASK = 0x3` → **2비트 = PORTA~PORTD 전부**.
   포트 A/B 제한은 **EM2/EM3 전용 제약**이고, 데이터시트 Table 3.3 은
   *"All GPIO ports support digital peripheral operation in EM0 and EM1"* 이라고 명시한다.
   스니퍼는 연속 RX = EM0/EM1 이고, `..._RESTRICT_ENERGY_MODE_TO_ALLOW_RECEPTION=1` 이
   이미 EM1 아래로 못 내려가게 잡고 있다 → **제약 없음**.
3. **클럭 심볼 존재 확인**: `CMU_CLKEN0_I2C0 = (1<<14)`, `cmuClock_I2C0`.

### 3.2 컴포넌트 선택 — `emlib_i2c` 하나만. **`i2cspm` 은 쓰지 마라**

| | **emlib_i2c ★** | i2cspm |
|---|---|---|
| 추가 파일 | `em_i2c.c` 1개 | `sl_i2cspm.c` + `em_i2c.c` |
| `config/` 새 파일 | **없음** | `sl_i2cspm_<inst>_config.h` 생성 |
| 핀툴 항목 | **불필요** | 필요 → **`config/pin_config.h` 재작성** |
| 인스턴스 | 없음 | 있음 |
| 전송 루프 | `I2C_TransferInit`/`I2C_Transfer` — **호출자가 돌린다** → 타임아웃을 우리가 건다 | 래퍼가 루프를 숨긴다 → 타임아웃 정책을 우리가 못 정한다 |

`i2cspm` 을 고르면 핀툴이 `config/pin_config.h` 를 재작성한다. 그 파일의 `$[EUSART0]` 블록에
**UART PB01(TX)/PB02(RX) 가 들어있다.** 건드릴 이유가 없다.
그리고 §6 의 타임아웃 요구는 **전송 루프를 우리가 소유해야** 지킬 수 있다.

### 3.3 ★먼저 할 것 — `.slcp` 와 config 헤더의 불일치부터 없앤다

**이것이 S14e 사고의 진짜 원인이다.** `config/sl_iostream_eusart_Vcom_config.h` 는 수기 편집되어
있는데, `.slcp` 가 그 값을 **모르거나 반대로 적고 있다.**

| 항목 | config 헤더 (현재·수기) | 템플릿 Default | `.slcp` (현재) | 재생성되면 |
|---|---|---|---|---|
| `..._BAUDRATE` | **921600** | 115200 | **항목 없음** | 115200 ❌ |
| `..._FLOW_CONTROL_TYPE` | **NONE** | NONE | **CTS_RTS** 🔴 | CTS/RTS ❌ |
| `..._RX_BUFFER_SIZE` | **1024** | 32 | **항목 없음** | 32 ❌ |

**플로 제어가 제일 위험하다.** `.slcp` 가 템플릿 기본값(NONE)보다 **더 나쁜 CTS_RTS 를 명시**하고 있다.
이 보드는 CTS/RTS 핀이 배선되어 있지 않으므로 CTS 가 영원히 assert 되지 않고 → **TX 가 통째로 멈춘다.**
로그도 앱도 같이 죽는다.

→ **I²C 를 추가하기 전에** `.slcp` 를 헤더와 같게 만든다. 그러면 autogen 이 돌아도 올바른 값이
돌아온다. 회피가 아니라 **근본 해결**이다.

### 3.4 `.slcp` 수정 지시 — 소장이 직접 (4건)

**(1) 플로 제어 — `CTS_RTS` → `NONE`** (`configuration:` 목록)

찾기:
```yaml
- {name: SL_IOSTREAM_USART_VCOM_FLOW_CONTROL_TYPE, value: usartHwFlowControlCtsAndRts}
- condition: [device_series_2]
  name: SL_IOSTREAM_EUSART_VCOM_FLOW_CONTROL_TYPE
  value: SL_IOSTREAM_EUSART_UART_FLOW_CTRL_CTS_RTS
- condition: [device_series_3]
  name: SL_IOSTREAM_EUSART_VCOM_FLOW_CONTROL_TYPE
  value: SL_IOSTREAM_EUSART_UART_FLOW_CTRL_CTS_RTS
```
바꾸기:
```yaml
- {name: SL_IOSTREAM_USART_VCOM_FLOW_CONTROL_TYPE, value: usartHwFlowControlNone}
- condition: [device_series_2]
  name: SL_IOSTREAM_EUSART_VCOM_FLOW_CONTROL_TYPE
  value: SL_IOSTREAM_EUSART_UART_FLOW_CTRL_NONE
- condition: [device_series_3]
  name: SL_IOSTREAM_EUSART_VCOM_FLOW_CONTROL_TYPE
  value: SL_IOSTREAM_EUSART_UART_FLOW_CTRL_NONE
```

**(2) 보율·수신버퍼 — 2줄 신규 추가** (위 3줄 바로 아래 권장)
```yaml
- {name: SL_IOSTREAM_EUSART_VCOM_BAUDRATE, value: '921600'}
- {name: SL_IOSTREAM_EUSART_VCOM_RX_BUFFER_SIZE, value: '1024'}
```

**(3) I²C 컴포넌트 — 1줄 추가** (`component:` 목록, `device_init` 다음)
```yaml
- {id: emlib_i2c}
```
Studio GUI 로 넣을 경우: Software Components → `I2C` 검색 → Platform 아래 **"I2C" (emlib)** → Install.
**인스턴스 이름을 묻지 않는 것이 정상.** 묻는다면 그건 I2CSPM 이니 취소한다.
※ SDK 2025.6 에서 id 문자열이 `emlib_i2c` 가 맞는지는 **Studio 가 `.slcp` 에 써넣는 값**으로 확인.

**(4) 핀툴은 건드리지 않는다.** `.pintool` 에 I²C 항목을 추가하지 않는다.
`config/pin_config.h` 의 `$[I2C0]` 블록은 **빈 채로 둔다.** 라우팅은 코드에서 한다(§3.5).

**수정 후 · 빌드 전 확인 3가지**
- `config/sl_iostream_eusart_Vcom_config.h` → `921600` / `..._NONE` / `1024` 가 **그대로인지**
- `config/pin_config.h` → `$[EUSART0]` 블록이 **PB01(TX)/PB02(RX) 그대로인지**
- `GNU ARM .../subdir.mk` → `emlib/src/em_i2c.c` 가 **추가됐는지**
- 하나라도 어긋나면 **되돌리고 보고.** 그냥 진행하지 마라

### 3.5 라우팅·클럭 — 코드에서 (Claude Code 담당)

```c
CMU->CLKEN0_SET = CMU_CLKEN0_I2C0;

/* 외부 풀업 R57/R58 10K 있음 → 내부 풀업 금지, open-drain */
GPIO_PinModeSet(BOARD_CUR_SCL_PORT, BOARD_CUR_SCL_PIN, gpioModeWiredAnd, 1);
GPIO_PinModeSet(BOARD_CUR_SDA_PORT, BOARD_CUR_SDA_PIN, gpioModeWiredAnd, 1);

/* ★버스 복구: 리셋 직전 전송 중이었다면 슬레이브가 SDA 를 물고 있다.
   I2C_Init 전에 SCL 9클럭 토글 + STOP 으로 풀어준다. */

GPIO->I2CROUTE[0].SCLROUTE =                      /* [0] = I2C0 */
    ((uint32_t)BOARD_CUR_SCL_PORT << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
  | ((uint32_t)BOARD_CUR_SCL_PIN  << _GPIO_I2C_SCLROUTE_PIN_SHIFT);
GPIO->I2CROUTE[0].SDAROUTE =
    ((uint32_t)BOARD_CUR_SDA_PORT << _GPIO_I2C_SDAROUTE_PORT_SHIFT)
  | ((uint32_t)BOARD_CUR_SDA_PIN  << _GPIO_I2C_SDAROUTE_PIN_SHIFT);
GPIO->I2CROUTE[0].ROUTEEN = GPIO_I2C_ROUTEEN_SCLPEN | GPIO_I2C_ROUTEEN_SDAPEN;

I2C_Init_TypeDef init = I2C_INIT_DEFAULT;         /* 100 kHz, master */
I2C_Init(I2C0, &init);
```

- `BOARD_CUR_SCL_PORT` 는 `gpioPortC`(=2) 로 들어간다. PORT 필드가 2비트이므로 그대로 맞는다.
- **★타임아웃**: `I2C_TransferInit()` 후 `I2C_Transfer()` 가 `i2cTransferInProgress` 를
  반환하는 동안 도는 루프에 **반드시 상한**을 둔다(권장 10 ms). 초과 시 중단 → `I2C_ERR` 카운트 →
  다음 1 Hz 주기에 재시도. 메인루프를 막으면 **워치독(2.05초)이 문다.**

---

## 4. 작업

### 4.1 축 신설 — `FS_INA`

```c
/* ★S11 INA219 전류·전압 감시 (FS_N 과 직교 — 별도 축, 누적형)
 *   0 = 미사용 (현행)
 *   1 = S11a  I2C 비트뱅 + 레지스터 읽기 + 배너/로그 출력   ← 이번
 *   2 = +S11b TYPE='B' 프레임 송신 (1Hz 주기 + 이벤트)
 *  회귀 시 0 으로 내리면 즉시 복귀한다. */
#define FS_INA   1
```

**`FS_N`(6) · `FS_POWER`(1) · `FS_WDOG`(1) 은 건드리지 마라.**

### 4.2 INA219 초기화 값 (설계문서 §3.3·§3.6 확정값)

| 레지스터 | 값 | 의미 |
|---|---|---|
| `0x00` Config | **`0x0FFF`** | BRNG **16V** · PGA **/2 (±80mV)** · BADC/SADC **12bit 128평균** · MODE shunt+bus **continuous** |
| `0x05` Calibration | **`16384`** (`0x4000`) | Current_LSB = **250 µA/bit**, Power_LSB = 5 mW/bit |

> ⚠ **리셋 기본값은 `0x399F`(32V · PGA ÷8)다.** 안 쓰면 PGA 가 /8 로 남아 설계값과 달라진다.
> **반드시 `0x0FFF` 를 써라.**
>
> ⚠ BRNG 는 **16V 가 맞다.** 12.6V 를 충분히 덮고 LSB 는 양쪽 모두 4 mV 로 같다.
> (대화 중 "32V 여야 한다"고 안내된 적이 있으나 그것은 오답으로 정정됐다)

### 4.3 읽기 변환식

| 레지스터 | 변환 |
|---|---|
| `0x01` Shunt | **signed 16bit**, LSB **10 µV**. `i_ma = raw_uV / 10` (R33=10mΩ 이므로 10µV = 1mA) |
| `0x02` Bus | **`(raw >> 3) × 4 mV`** ← ★하위 3비트를 먼저 버린다. bit1=CNVR, bit0=OVF |
| `0x04` Current | `raw × 250 µA` (Calibration 쓴 뒤에만 유효) |

**`v_sys_mv` 는 계산으로 얻는다**: `v_sys_mv = v_pack_mv + vshunt_mv`
(Bus 는 `VIN−` = Bat+ 를 보므로 그 자체가 **팩 전압**이다)

### 4.4 출력 (S11a 범위 — 사람이 읽는 로그)

부팅 배너에 1회 + 이후 **1초 주기**로:

```
[S11a] INA219 cfg=0x0FFF(rb) cal=16384(rb)  addr=0x40 OK
[S11a] vpack=12580mV  vsys=12586mV  i=+00590mA  shunt=+5900uV  (충전)
```

- `(rb)` = **write 후 read-back 값**. 일치해야 I²C 가 정말 성립한 것이다
- I²C 실패 시: `[S11a] I2C ERR step=<어디서> nack=<n>` — **조용히 0 을 찍지 마라**

---

## 5. 검증 (완료조건)

| # | 시험 | 합격 |
|---|---|---|
| 1 | **I²C 성립** — Config/Calibration write 후 read-back | `0x0FFF` / `16384` **일치** |
| 2 | **무부하** (가능하면) | `i_ma ≈ 0` (±수 mA). 크면 켈빈 배선·오프셋 확인 |
| 3 | **방전** (배터리 동작 중) | `i_ma ≈ **−700 mA**`, **부호 음수** ✓ |
| 4 | **충전** (어댑터 연결) | `i_ma` **양수 전환**. CC 구간 정격 근처 → CV 로 넘어가며 **감소** |
| 5 | **Bus 전압** | 팩 전압과 일치. 충전 중 12.5~12.6V 고정이면 CV 모드 |
| 6 | **★기준기 대조** | DMM 직렬 또는 클램프와 **±3% 이내** (요구사항 §7-9) |
| 7 | **★`R33` 발열** | **충전 중** 션트 표면온도. 3216(1206) 패키지 정격 대비 여유 확인 (`회로검토 2V0` 잔존 지적) |
| 8 | **★RF 영향** | I²C 감시 on/off 로 **RSSI floor 비교 ≤ 1 dB** (설계문서 §6-5). I²C 는 100kHz 구형파라 고조파가 넓게 퍼진다 |
| 9 | **회귀** | 기존 배너 전항목 + `[S6a] start_rx ch=101 OK` 유지 |

### ★5-10. 이번 스텝의 진짜 목적 — 창(window) 실측

```
BAT_CRITICAL 후보 전압(9.0V)에서 부하 0.7A 를 걸고
8.4V(팩 BMS 차단)까지 몇 초가 걸리는가?
```

- 이 값이 `§0.2` 의 **상한**이고, **`PWROFF` 대기시간의 최대치**다
- **상한 < Android 종료 시간이면 → 대기시간을 늘리는 게 아니라 `BAT_CRITICAL` 임계를 올린다**
  (9.0V → 9.3~9.6V 검토). 3S 리튬이온은 셀 3.0V 아래에서 급강하한다
- 장시간 시험이라 S11a 통과 후 별도로 잡아도 된다. **다만 반드시 잰다**

---

## 6. ★안전

- **RF 송신 없음** — I²C 는 수동 읽기다. 안전 절대선과 무관 ✅
- **★모든 I²C 대기에 타임아웃을 건다.** 근거: `작업정리_S14a_S14e` §3 이
  *"메인루프에 블로킹 구간 없음"* 을 전제로 워치독 주기를 2.05초로 잡았다.
  I²C 가 버스 행(hang)으로 무한 대기하면 **그 전제가 깨져 워치독이 문다.**
  → 비트당 타임아웃 + 트랜잭션 전체 타임아웃. **1회 트랜잭션은 1 ms 이내여야 한다**
- **`POW_EN_CON` 을 LOW 로 만드는 코드 금지** (S14a 규칙 유지 — 자폭 경로)
- **버스 복구**: NACK/타임아웃 시 SCL 을 9클럭 토글해 슬레이브를 풀어주고 STOP. 그래도 안 되면
  **에러를 보고하고 다음 주기에 재시도**. 무한 재시도 금지

---

## 7. 하지 말 것

- ❌ **`.slcp` · `.pintool` · `config/` 를 네가 고치지 마라** — 소장이 직접 수정한다(§3.4).
  수정 후 `config/sl_iostream_eusart_Vcom_config.h`(921600/NONE/1024)와
  `config/pin_config.h`($[EUSART0] = PB01/PB02)가 **그대로인지 확인**하는 것까지가 네 일이다
- ❌ **`TYPE='B'` 프레임 송신 금지** → **S11b**. 이번엔 사람이 읽는 로그까지만
- ❌ **내부 풀업 금지** (외부 R57/R58 10K)
- ❌ **push-pull 로 SCL/SDA 구동 금지** — open-drain(`gpioModeWiredAnd`)
- ❌ **부호 반전 금지** — `+`=충전이 설계대로이고 실측으로 확인됐다
- ❌ `FS_N` · `FS_POWER` · `FS_WDOG` 변경 금지
- ❌ `ADAPTER_IN`(PC04) · `KEY`(PC00) 건드리지 마라 → S12 / S14b

---

## 8. 보고할 것

1. **§5 시험 1~9 결과** (특히 6번 기준기 대조 수치, 7번 션트 온도)
2. 1회 트랜잭션 소요시간 실측 + 타임아웃 상한값과 그 근거
3. `.bin` 크기 변경분 (이번엔 코드가 늘어나므로 **크기가 달라지는 게 정상**)
4. `_known_good/S11a_<날짜>.s37` 앵커 + `step_config.h` 이력 1줄
5. **§5-10 창(window) 측정 결과** — 별도로 해도 되지만 언제 할지 명시
6. `../Network_Analyzer/FG23_UART_프로토콜_v2_확장안_20261014.md §5` 스텝표에 S11a 완료 표시

---

## 부록 — 복사용 요청 문장 (클로드코드에 그대로 붙여넣기)

```
NetAnalyzer_FG23_V01 에 S11a (INA219 하드웨어 I2C 읽기) 를 구현한다.
먼저 `작업지시_ClaudeCode_S11a_INA219_20261016.md` 를 §3 포함해 끝까지 정독해라.

구현 방식은 **하드웨어 I2C0 로 확정**됐다. 비트뱅 검토는 끝났으니 다시 꺼내지 마라.

진행은 3단계다. **각 단계 끝에서 멈추고 보고**해라. 내 승인 없이 다음 단계로 넘어가지 마라.

═══ [1단계] .slcp 수정 — 텍스트 편집만. Studio GUI 를 쓰지 마라 ═══

대상: Software/NetAnalyzer_FG23_V01/NetAnalyzer_FG23_V01.slcp
먼저 NetAnalyzer_FG23_V01.slcp.bak 으로 원본을 복사해라.

(1) configuration: 목록 — 플로 제어 3건을 NONE 으로
      usartHwFlowControlCtsAndRts
        -> usartHwFlowControlNone
      SL_IOSTREAM_EUSART_UART_FLOW_CTRL_CTS_RTS
        -> SL_IOSTREAM_EUSART_UART_FLOW_CTRL_NONE
      (device_series_2 / device_series_3 조건 두 줄 모두)

(2) configuration: 목록에 2줄 신규 추가
      - {name: SL_IOSTREAM_EUSART_VCOM_BAUDRATE, value: '921600'}
      - {name: SL_IOSTREAM_EUSART_VCOM_RX_BUFFER_SIZE, value: '1024'}

(3) component: 목록에 1줄 추가 (device_init 다음, 알파벳 순)
      - {id: emlib_i2c}
    ※ 이 id 문자열이 SDK 2025.6 에서 맞는지 확신 없으면 추가만 해두고 보고해라.
      빌드에서 컴포넌트를 못 찾으면 내가 Studio 에서 확인한다.

(4) .pintool 은 건드리지 마라. config/pin_config.h 의 $[I2C0] 블록은 빈 채로 둔다.
    I2C 핀 라우팅은 핀툴이 아니라 코드에서 GPIO->I2CROUTE[0] 로 한다.

★(1)(2)를 하는 이유 — 이게 S14e 사고의 진짜 원인이다:
  config/sl_iostream_eusart_Vcom_config.h 는 수기로 921600 / FLOW_CTRL_NONE / RX 1024 로
  고쳐져 있는데, .slcp 는 보율·버퍼를 아예 모르고 플로제어는 거꾸로 CTS_RTS 라고 적고 있다.
  이 보드엔 CTS/RTS 핀이 배선돼 있지 않다. 재생성으로 CTS_RTS 가 복원되면 CTS 가 영원히
  assert 되지 않아 TX 가 통째로 멈춘다(로그·앱 동시 사망).
  .slcp 를 헤더와 같게 맞춰두면 autogen 이 돌아도 올바른 값이 돌아온다. 회피가 아니라 근본 해결.

→ 1단계가 끝나면 .slcp diff 를 보여주고 **멈춰라. 빌드하지 마라.**

═══ [2단계] 재생성 확인 — 내가 Studio 에서 한 번 빌드한 뒤 ═══

내가 Studio 를 열어 빌드하면, 그 다음에 아래를 확인해라. **고치려 들지 말고 확인만** 해라.

  a) config/sl_iostream_eusart_Vcom_config.h
       -> 921600 / SL_IOSTREAM_EUSART_UART_FLOW_CTRL_NONE / RX_BUFFER_SIZE 1024 그대로인가
  b) config/pin_config.h
       -> $[EUSART0] 블록이 PB01(TX) / PB02(RX) 그대로인가
  c) GNU ARM v12.2.1 - Default/subdir.mk
       -> emlib/src/em_i2c.c 가 추가됐는가

하나라도 어긋나면 **멈추고 보고**해라.

═══ [3단계] S11a 구현 ═══

- I2C0 = PC02(SCL) / PC03(SDA), slave 0x40, 외부 풀업 R57/R58 10K 있음
- 클럭: CMU->CLKEN0_SET = CMU_CLKEN0_I2C0  (기존 생성자의 GPIO 클럭 활성화와 같은 방식)
- 핀 모드: open-drain(gpioModeWiredAnd), 초기 HIGH. **내부 풀업 금지**
- 라우팅: GPIO->I2CROUTE[0].SCLROUTE / .SDAROUTE / .ROUTEEN  (§3.5 코드 그대로)
  [0] 이 I2C0 다. PORT 필드는 2비트이고 gpioPortC(=2)가 그대로 들어간다
- ★I2C_Init 전에 버스 복구: SCL 9클럭 토글 + STOP.
  리셋 직전 전송 중이었으면 슬레이브가 SDA 를 LOW 로 물고 있다
- INA219 Config = 0x0FFF (BRNG 16V / PGA /2 / 12bit 128평균 / continuous)
  Calibration = 16384 (Current_LSB 250uA)
  ★리셋 기본값 0x399F 를 그대로 두면 PGA 가 /8 로 남는다. 반드시 덮어써라
- Bus(0x02) 변환은 (raw >> 3) * 4mV. **하위 3비트를 먼저 버려라**
- 극성은 + = 충전 / - = 방전. 2026-10-14 실측(+5.9mV = 충전 590mA)으로 확인됐다.
  **뒤집지 마라**
- ★I2C_Transfer 펌프 루프에 반드시 상한(권장 10ms, sleeptimer tick 기준).
  초과 시 중단 -> I2C_ERR 카운트 -> 다음 1Hz 주기에 재시도. 무한 재시도 금지.
  메인루프를 막으면 워치독(2.05초)이 문다
- 이번 범위는 **사람이 읽는 로그까지만**. TYPE='B' 프레임 송신은 S11b 다
- FS_INA 축을 새로 만들어 1 로. FS_N / FS_POWER / FS_WDOG 는 건드리지 마라
- POW_EN_CON 을 LOW 로 만드는 코드 금지 (S14a 자폭 경로)
- ADAPTER_IN(PC04) / KEY(PC00) 건드리지 마라 -> S12 / S14b

구현 착수 전에 **I2C 드라이버 구조(함수 목록 + 타임아웃 설계 + 버스복구 절차)를 먼저 보고**해라.
```

---

### 1단계를 Studio GUI 로 직접 하실 경우

위 블록에서 **[1단계] 전체를 지우고** 클로드코드에는 2·3단계만 준다.
GUI 경로: Software Components → `I2C` 검색 → Platform 아래 **"I2C" (emlib)** → Install.
**인스턴스 이름을 묻지 않는 것이 정상**이고, 묻는다면 그건 I2CSPM 이므로 취소한다.
플로제어·보율·버퍼는 GUI 로는 안 보이므로 `.slcp` 를 직접 열어 (1)(2)를 손으로 고친다.
