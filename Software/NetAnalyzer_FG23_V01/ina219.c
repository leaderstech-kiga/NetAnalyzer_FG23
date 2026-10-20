/***************************************************************************//**
 * @file    ina219.c
 * @brief   S11a — INA219 전류·전압 감시 (하드웨어 I2C0)
 *
 *  설계 근거는 ina219.h 헤더 주석과 `작업지시_ClaudeCode_S11a_INA219_20261016.md`.
 *  여기에는 **구현 판단의 이유**만 적는다.
 ******************************************************************************/

#include "step_config.h"                    /* FS_INA / FS_N 스텝 가드 */

#if (FS_INA >= 1) && (FS_N >= 1)

#include "em_device.h"                      /* GPIO->I2CROUTE / CMU 레지스터 */
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_i2c.h"
#include <stddef.h>                         /* offsetof — 라우팅 오프셋 검증 */
#include "sl_sleeptimer.h"

#include "app_log.h"
#include "board_cfg.h"
#include "ina219.h"

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

/* --- INA219 슬레이브 주소 ---------------------------------------------------
 *  A0/A1 이 R59/R65 로 GND 풀다운 → 7bit **0x40**.
 *  ★em_i2c 의 `I2C_TransferSeq_TypeDef.addr` 는 "AAAA AAAX" 형식(= 7bit 를 1칸
 *   왼쪽으로 민 8bit)을 요구한다 (em_i2c.h:244). 여기서 미리 밀어 둔다 —
 *   전송할 때마다 밀면 한 군데에서 빠뜨린다. */
#define INA219_ADDR_7B        0x40u
#define INA219_ADDR_SEQ       ((uint16_t)(INA219_ADDR_7B << 1))   /* = 0x80 */

/* --- 레지스터 맵 (INA219 데이터시트 Table 7) -------------------------------- */
#define INA219_REG_CONFIG     0x00u
#define INA219_REG_SHUNT      0x01u
#define INA219_REG_BUS        0x02u
#define INA219_REG_CURRENT    0x04u
#define INA219_REG_CAL        0x05u

/* --- 설정값 (설계문서 §3.3·§3.6 확정 / 작업지시 §4.2) -----------------------
 *  Config 0x0FFF = BRNG 16V · PGA /2(±80mV) · BADC/SADC 12bit 128평균 ·
 *                  MODE shunt+bus continuous
 *   ⚠ **리셋 기본값은 0x399F(32V · PGA ÷8)다.** 덮어쓰지 않으면 PGA 가 /8 로
 *     남아 설계값과 달라진다 — 조용히 틀린 값이 나오는 유형이라 제일 나쁘다.
 *   ⚠ BRNG 는 **16V 가 맞다.** 12.6V 를 충분히 덮고 LSB 는 양쪽 모두 4mV 로
 *     같다. (대화 중 "32V 여야 한다"는 안내가 있었으나 오답으로 정정됨)
 *
 *  Calibration 16384 = Current_LSB 250µA/bit, Power_LSB 5mW/bit */
#define INA219_CONFIG_VAL     0x0FFFu
#define INA219_CAL_VAL        16384u

/* --- 주기 -------------------------------------------------------------------
 *  ★S1 틱(1000ms)과 **위상을 500ms 어긋나게** 둔다. 같은 루프 반복에 겹치면
 *   링버퍼가 가장 찬 순간에 I2C 블로킹이 얹힌다. 공짜로 피할 수 있는 겹침이다. */
#define INA_PERIOD_MS         1000u
#define INA_PHASE_MS          500u

/* --- ★타임아웃 (작업지시 §3.5·§6) -------------------------------------------
 *  1층 = 시간 상한. 권장값 10ms 를 그대로 쓴다.
 *   계산상 1회 트랜잭션은 0.4~0.5ms(100kHz, 38~47bit)라 20배 여유다.
 *   **실측 후 조일 것** — 지금 미리 줄이면 정상 동작을 타임아웃으로 오판한다.
 *
 *  2층 = 반복 상한. 1층이 `sl_sleeptimer` 에 의존하므로, **sleeptimer 자체가
 *   멈추는 고장**이면 1층이 무력화된다. 그때 메인루프가 영구 정지하면 워치독이
 *   물고, 워치독 리셋이 전원 래치를 어떻게 건드리는지는 아직 실측 전(T6 미시험)
 *   이다. 보험이 싸다. 200000 회는 39MHz 에서 수십 ms 규모 — 1층이 정상이면
 *   절대 도달하지 않는다. */
#define INA_XFER_TIMEOUT_MS   10u
#define INA_XFER_GUARD_ITERS  200000u

/* i2c_xfer() 전용 반환값. em_i2c 의 음수 코드(-1~-5)와 겹치지 않게 뗀다. */
#define INA_RC_TIMEOUT        (-10)
#define INA_RC_GUARD          (-11)

/* --- 버스 복구 클럭 반주기 (busy loop) --------------------------------------
 *  ★정밀도가 필요 없는 자리다. 복구 클럭은 1~100kHz 아무 데나 들어가면 되고
 *   **느릴수록 안전**하다. 39MHz 에서 volatile 루프 1회 ≈ 5사이클 → 100회 ≈ 13µs.
 *   (생성자의 BOARD_TCXO_SETTLE_LOOPS 와 같은 성격) */
#define INA_RECOVER_HALF_LOOPS  100u

/* 에러 로그 폭주 방지: 처음 N 회는 매번, 이후 M 회에 1번만. */
#define INA_ERRLOG_ALWAYS     5u
#define INA_ERRLOG_EVERY      60u

/* ★라우팅 레지스터 필드 폭 검증 (컴파일 타임).
 *  PORT 는 2bit(0x3) — gpioPortC(=2)까지만 들어간다. 포트 D 이상을 쓰려 들면
 *  조용히 잘리는 대신 빌드가 깨지도록 한다. (작업지시 §3.5 "PORT 필드는 2비트") */
_Static_assert(((uint32_t)BOARD_CUR_SCL_PORT << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
               <= _GPIO_I2C_SCLROUTE_PORT_MASK,
               "SCL 포트가 I2CROUTE PORT 필드(2bit)를 넘는다");
_Static_assert(((uint32_t)BOARD_CUR_SDA_PIN << _GPIO_I2C_SDAROUTE_PIN_SHIFT)
               <= _GPIO_I2C_SDAROUTE_PIN_MASK,
               "SDA 핀이 I2CROUTE PIN 필드(4bit)를 넘는다");

/* ★`I2CROUTE[0]` 이 정말 I2C0 인가 — 레지스터 오프셋으로 못 박는다.
 *  "[0] 이 I2C0 이겠지"는 **추측**이다. SDK 가 구조체를 바꾸면 조용히 I2C1 을
 *  설정하게 되고, 그때 증상은 "통신이 안 된다" 하나뿐이라 원인을 찾기 어렵다.
 *  S14e 에서 WDOG 레지스터 오프셋 12개를 못 박았던 것과 같은 이유다.
 *  (레퍼런스 매뉴얼 GPIO 레지스터 맵: I2C0_ROUTEEN=0x538 / I2C1_ROUTEEN=0x548) */
_Static_assert(offsetof(GPIO_TypeDef, I2CROUTE[0].ROUTEEN)  == 0x538u, "I2CROUTE[0] != I2C0");
_Static_assert(offsetof(GPIO_TypeDef, I2CROUTE[0].SCLROUTE) == 0x53Cu, "SCLROUTE 오프셋");
_Static_assert(offsetof(GPIO_TypeDef, I2CROUTE[0].SDAROUTE) == 0x540u, "SDAROUTE 오프셋");
_Static_assert(offsetof(GPIO_TypeDef, I2CROUTE[1].ROUTEEN)  == 0x548u, "I2CROUTE[1] != I2C1");

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------

static uint64_t s_next_tick;        /* 다음 측정 시각 */
static bool     s_ready;            /* I2C·INA219 가 성립한 상태인가 */
static uint32_t s_err_count;        /* 누적 I2C 실패 (진단) */
static uint32_t s_errlog_seq;       /* 에러 로그 폭주 방지 카운터 */

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------

static void i2c_pins_init(void);
static void i2c_bus_recover(void);
static void i2c_route_enable(void);
static bool i2c_hw_setup(void);
static int  i2c_xfer(I2C_TransferSeq_TypeDef *seq);
static int  ina_reg_write(uint8_t reg, uint16_t val);
static int  ina_reg_read(uint8_t reg, uint16_t *val);
static bool ina_configure(void);
static void err_report(const char *step, int rc);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void ina219_init(void)
{
  /* ★버스 클럭. 생성자가 GPIO 를 켠 것과 같은 방식이다 (CLKEN0_SET).
   *  ⚠ 이것은 **버스 클럭**이고 기능 클럭과 별개다. I2C0 의 기능 클럭은
   *   `LSPCLK = HCLK / PCLKDIV / 2` ≈ 19.5MHz 로 **고정**이며, 워치독과 달리
   *   `I2C0CLKCTRL` 같은 선택 레지스터가 **아예 없다**(efr32fg23_cmu.h 확인).
   *   → S14e 에서 물릴 뻔했던 "기능 클럭 기본값" 함정이 여기엔 없다.
   *   em_i2c 는 이 값을 CMU_ClockFreqGet(cmuClock_I2C0) 로 가져간다
   *   (em_i2c.c:293 → em_cmu.c:1024 의 cmuClock_LSPCLK 분기). */
  CMU->CLKEN0_SET = CMU_CLKEN0_I2C0;

  s_ready = i2c_hw_setup() && ina_configure();

  /* 첫 측정은 INA_PHASE_MS 뒤. (S1 틱과 위상 분리) */
  s_next_tick = sl_sleeptimer_get_tick_count64()
                + sl_sleeptimer_ms_to_tick(INA_PHASE_MS);

  if (!s_ready) {
    /* 부팅을 막지 않는다 — 1Hz 주기에서 재시도한다. 전류 감시가 안 되는 것과
     *  스니퍼가 안 뜨는 것은 심각도가 다르다. */
    app_log("[S11a] INA219 init FAIL -- retry at 1Hz (err=%lu)\r\n",
            (unsigned long)s_err_count);
  }
}

void ina219_poll(void)
{
  uint64_t now = sl_sleeptimer_get_tick_count64();

  if (now < s_next_tick) {
    return;                                   /* 대부분의 호출은 여기서 끝 */
  }
  s_next_tick = now + sl_sleeptimer_ms_to_tick(INA_PERIOD_MS);

  /* --- 끊긴 상태면 복구부터. 주기당 1회만 시도한다(무한 재시도 금지) --- */
  if (!s_ready) {
    s_ready = i2c_hw_setup() && ina_configure();
    if (!s_ready) {
      return;                                 /* 로그는 err_report 가 이미 냈다 */
    }
    app_log("[S11a] INA219 recovered (err total=%lu)\r\n",
            (unsigned long)s_err_count);
  }

  /* --- 측정 3건 --------------------------------------------------------- */
  uint16_t raw_shunt = 0u, raw_bus = 0u, raw_cur = 0u;
  int rc;

  rc = ina_reg_read(INA219_REG_SHUNT, &raw_shunt);
  if (rc != i2cTransferDone) { err_report("shunt_read", rc); s_ready = false; return; }

  rc = ina_reg_read(INA219_REG_BUS, &raw_bus);
  if (rc != i2cTransferDone) { err_report("bus_read", rc);   s_ready = false; return; }

  rc = ina_reg_read(INA219_REG_CURRENT, &raw_cur);
  if (rc != i2cTransferDone) { err_report("cur_read", rc);   s_ready = false; return; }

  /* --- 변환 (작업지시 §4.3) ---------------------------------------------
   *  Shunt(0x01) : **signed 16bit**, LSB 10µV
   *     → shunt_uv = (int16)raw * 10
   *     → R33 = 10mΩ 이므로 I = V/R = 10µV / 0.01Ω = **1mA per LSB**
   *       즉 i_ma 는 raw 그대로다. (우연이 아니라 션트값을 그렇게 고른 것)
   *  Bus(0x02)   : ★**하위 3비트를 먼저 버린다.** bit1=CNVR, bit0=OVF
   *     → v_pack_mv = (raw >> 3) * 4mV
   *     → Bus 는 VIN− = Bat+ 를 보므로 그 자체가 **팩 전압**이다
   *  V_SYS       : 계산으로 얻는다 — v_sys = v_pack + vshunt */
  int16_t  shunt_raw = (int16_t)raw_shunt;
  int32_t  shunt_uv  = (int32_t)shunt_raw * 10;
  int32_t  i_ma      = (int32_t)shunt_raw;          /* 10µV / 10mΩ = 1mA */
  uint32_t v_pack_mv = ((uint32_t)raw_bus >> 3) * 4u;
  int32_t  v_sys_mv  = (int32_t)v_pack_mv + (shunt_uv / 1000);  /* 소수점 이하 버림 */

  /* ★Current 레지스터(0x04) 교차검증.
   *  0x04 는 Calibration 을 쓴 뒤에만 유효하다 → 이 값이 션트 계산과 맞으면
   *  **Calibration 레지스터가 살아있다는 상시 증거**가 된다. 출력은 하지 않고
   *  어긋날 때만 경고한다(로그를 두 배로 늘리지 않으려고).
   *  허용오차: 2% + 2mA. 2mA 상수는 0 근처에서 퍼센트가 무의미해지기 때문. */
  int32_t  i_ma_reg = ((int32_t)(int16_t)raw_cur) / 4;    /* 250µA/bit = 0.25mA */
  int32_t  diff     = (i_ma > i_ma_reg) ? (i_ma - i_ma_reg) : (i_ma_reg - i_ma);
  int32_t  tol      = (((i_ma < 0) ? -i_ma : i_ma) / 50) + 2;   /* 2% + 2mA */

  /* --- 출력 (작업지시 §4.4) --------------------------------------------- */
  /* ★로그 본문은 **ASCII 전용**이다 (app_log.h 헤더 규약).
   *  [원본 -- "충전"/"방전"/"무부하"] 2026-10-16 실측에서 터미널 인코딩이 달라
   *  `(������)` 로 깨져 나왔다. 1초마다 반복되는 줄이라 터미널 설정에 기대면
   *  안 된다. 한글은 부팅 1회성 안내에만 쓰고, 주기 출력은 ASCII 로 고정한다.
   *
   *  ★raw= 는 2026-10-16 진단용으로 추가했다. "배터리 단독 구동인데 부호가 +" 를
   *   쫓을 때 **변환식을 용의선상에서 빼려면 원시값이 보여야** 한다.
   *   s=Shunt(0x01) b=Bus(0x02) c=Current(0x04) — 전부 레지스터 그대로. */
  /* ★t= 는 §5-10 창(window) 측정용으로 2026-10-16 추가했다.
   *  그 전까지는 `[S1] tick=N` 을 1초 시계로 **추정**해 썼는데, 그 간격은
   *  `next = now + 1000ms` 라 "최소 1000ms" 이지 정확히 1000ms 가 아니다.
   *  루프 지연만큼 매번 길어져 몇 시간짜리 로그에서 수 초가 어긋난다.
   *  시간을 재는 시험에서 시간만 추정값인 건 말이 안 된다. (추측보다 실측)
   *  ※ 64bit 변환을 쓰는 이유: 32bit tick_to_ms 는 32768Hz 에서 **36시간**에
   *    넘친다. 장시간 방전시험이 그 근처까지 갈 일은 없지만, 넘치는 순간
   *    시간이 거꾸로 가는 로그는 디버깅을 통째로 망친다. */
  uint64_t t_ms64 = 0u;
  (void)sl_sleeptimer_tick64_to_ms(now, &t_ms64);
  /* 출력은 32bit 로 줄인다. nano-printf 가 %llu 를 지원하지 않을 수 있고,
   * 32bit ms 는 49일이라 이 장비의 연속가동 시간보다 훨씬 길다. */
  unsigned long t_ms = (unsigned long)(uint32_t)t_ms64;

  app_log("[S11a] t=%lums vpack=%lumV vsys=%ldmV i=%+06ldmA shunt=%+lduV (%s) "
          "raw s=%04X b=%04X c=%04X\r\n",
          t_ms,
          (unsigned long)v_pack_mv, (long)v_sys_mv,
          (long)i_ma, (long)shunt_uv,
          (i_ma > 0) ? "CHG" : ((i_ma < 0) ? "DIS" : "IDLE"),
          (unsigned)raw_shunt, (unsigned)raw_bus, (unsigned)raw_cur);

  if (diff > tol) {
    app_log("[S11a] note current reg mismatch shunt=%ldmA reg=%ldmA "
            "-- check Calibration(0x05)\r\n", (long)i_ma, (long)i_ma_reg);
  }
}

uint32_t ina219_err_count(void)
{
  return s_err_count;
}

bool ina219_is_ready(void)
{
  return s_ready;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

/***************************************************************************//**
 * 핀을 open-drain 으로. **내부 풀업을 켜지 않는다.**
 *  외부 R57/R58 = 10kΩ 이 이미 있다. 내부 풀업을 겹치면 등가 저항이 내려가
 *  상승엣지는 빨라지지만 LOW 구동 전류가 늘고, 무엇보다 **설계값이 아니게 된다**.
 *  (작업지시 §7 "내부 풀업 금지" / "push-pull 구동 금지")
 ******************************************************************************/
static void i2c_pins_init(void)
{
  GPIO_PinModeSet(BOARD_CUR_SCL_PORT, BOARD_CUR_SCL_PIN, gpioModeWiredAnd, 1);
  GPIO_PinModeSet(BOARD_CUR_SDA_PORT, BOARD_CUR_SDA_PIN, gpioModeWiredAnd, 1);
}

/***************************************************************************//**
 * ★버스 복구 — **라우팅을 켜기 전에** 해야 한다.
 *
 *  리셋 직전에 읽기 전송 중이었다면 슬레이브가 ACK/데이터 비트를 내보내는
 *  도중에 마스터만 리셋된다. 그러면 슬레이브는 **SDA 를 LOW 로 문 채** 다음
 *  SCL 엣지를 기다리고, 마스터는 START 를 못 만든다 → 영구 교착.
 *  SCL 을 9클럭 주면 슬레이브가 바이트를 다 내보내고 손을 떼고, 그 뒤 STOP 으로
 *  상태기를 초기화한다.
 *
 *  ★순서가 핵심: 라우팅(I2CROUTE)을 먼저 켜면 핀 소유권이 I2C 페리페럴로
 *   넘어가 **GPIO 로 토글할 수 없다.** 그래서 핀모드 → 복구 → 라우팅 순이다.
 *
 *  조건 없이 항상 돈다(≈ 90µs, 부팅 1회). SDA 상태를 읽어 분기할 수도 있지만,
 *  출력 모드에서 DIN 이 패드 상태를 보여주는지에 **의존하고 싶지 않다.**
 *  판정은 로그로만 남긴다 — 추측을 코드 경로에 넣지 않는다.
 ******************************************************************************/
static void i2c_bus_recover(void)
{
  bool sda_was_low = (GPIO_PinInGet(BOARD_CUR_SDA_PORT, BOARD_CUR_SDA_PIN) == 0u);

  for (unsigned i = 0; i < 9u; i++) {
    GPIO_PinOutClear(BOARD_CUR_SCL_PORT, BOARD_CUR_SCL_PIN);
    for (volatile uint32_t d = 0; d < INA_RECOVER_HALF_LOOPS; d++) { }
    GPIO_PinOutSet(BOARD_CUR_SCL_PORT, BOARD_CUR_SCL_PIN);
    for (volatile uint32_t d = 0; d < INA_RECOVER_HALF_LOOPS; d++) { }
  }

  /* STOP 생성: SDA LOW → SCL HIGH → SDA HIGH (SCL HIGH 중 SDA 상승 = STOP) */
  GPIO_PinOutClear(BOARD_CUR_SDA_PORT, BOARD_CUR_SDA_PIN);
  for (volatile uint32_t d = 0; d < INA_RECOVER_HALF_LOOPS; d++) { }
  GPIO_PinOutSet(BOARD_CUR_SCL_PORT, BOARD_CUR_SCL_PIN);
  for (volatile uint32_t d = 0; d < INA_RECOVER_HALF_LOOPS; d++) { }
  GPIO_PinOutSet(BOARD_CUR_SDA_PORT, BOARD_CUR_SDA_PIN);
  for (volatile uint32_t d = 0; d < INA_RECOVER_HALF_LOOPS; d++) { }

  if (sda_was_low) {
    /* 실제로 물려 있었다는 뜻이다. 자주 뜨면 리셋 타이밍을 의심해야 한다. */
    app_log("[S11a] bus recover: SDA was LOW (slave held it)\r\n");
  }
}

/***************************************************************************//**
 * 핀 라우팅. ★핀툴(.pintool)을 쓰지 않고 코드에서 한다 (작업지시 §3.4-4).
 *  핀툴을 쓰면 `config/pin_config.h` 가 재작성되는데 거기에 UART PB01/PB02 가
 *  들어 있다. 건드릴 이유가 없다. → `$[I2C0]` 블록은 빈 채로 둔다.
 *
 *  `I2CROUTE[0]` 가 I2C0 다 (efr32fg23_gpio.h:245 `I2CROUTE[2U]`).
 ******************************************************************************/
static void i2c_route_enable(void)
{
  GPIO->I2CROUTE[0].SCLROUTE =
      ((uint32_t)BOARD_CUR_SCL_PORT << _GPIO_I2C_SCLROUTE_PORT_SHIFT)
    | ((uint32_t)BOARD_CUR_SCL_PIN  << _GPIO_I2C_SCLROUTE_PIN_SHIFT);

  GPIO->I2CROUTE[0].SDAROUTE =
      ((uint32_t)BOARD_CUR_SDA_PORT << _GPIO_I2C_SDAROUTE_PORT_SHIFT)
    | ((uint32_t)BOARD_CUR_SDA_PIN  << _GPIO_I2C_SDAROUTE_PIN_SHIFT);

  GPIO->I2CROUTE[0].ROUTEEN = GPIO_I2C_ROUTEEN_SCLPEN | GPIO_I2C_ROUTEEN_SDAPEN;
}

/***************************************************************************//**
 * 하드웨어 준비 일체. **실패 복구 시에도 이 함수를 통째로 다시 부른다** —
 * 부분 복구는 "어디까지 살아있나"를 추측하게 만든다.
 ******************************************************************************/
static bool i2c_hw_setup(void)
{
  i2c_pins_init();
  i2c_bus_recover();          /* ★라우팅 전에 */
  I2C_Reset(I2C0);            /* 페리페럴을 알려진 상태로 (전송 중 리셋 대비) */
  i2c_route_enable();

  I2C_Init_TypeDef init = I2C_INIT_DEFAULT;   /* 100kHz, master, 4:4 duty */
  I2C_Init(I2C0, &init);

  /* CLKDIV 가 0 이면 기준 클럭을 못 얻은 것이다 (CMU_ClockFreqGet 실패).
   *  이 경우 전송은 NACK 이 아니라 **엉뚱한 속도**로 나가 진단이 어려워진다.
   *  → 설계 보고 §10-③ 의 "2. 클럭" 분기를 코드가 직접 걸러준다. */
  if ((I2C0->CLKDIV & _I2C_CLKDIV_DIV_MASK) == 0u) {
    app_log("[S11a] I2C0 CLKDIV=0 -- ref clock failed (check LSPCLK)\r\n");
    s_err_count++;
    return false;
  }
  return true;
}

/***************************************************************************//**
 * ★전송의 **유일한 창구**. 타임아웃 정책이 여기 한 곳에만 있어야
 *  나중에 값을 조일 때 한 군데만 고친다.
 *
 *  1층 = 시간 상한(INA_XFER_TIMEOUT_MS)
 *  2층 = 반복 상한(INA_XFER_GUARD_ITERS) — sleeptimer 가 멈춰도 빠져나온다
 *
 *  ⚠ 메인루프를 막는 구간이다. 유계(최악 10ms)이고 워치독 2049ms 대비 200배
 *    여유지만, **"블로킹 없음"이 아니라 "10ms 이하로 유계"** 라는 점을
 *    `작업정리_S14a_S14e §3` 에 반영해야 한다.
 ******************************************************************************/
static int i2c_xfer(I2C_TransferSeq_TypeDef *seq)
{
  I2C_TransferReturn_TypeDef rc = I2C_TransferInit(I2C0, seq);

  uint64_t t0      = sl_sleeptimer_get_tick_count64();
  uint64_t limit   = sl_sleeptimer_ms_to_tick(INA_XFER_TIMEOUT_MS);
  uint32_t guard   = 0u;

  while (rc == i2cTransferInProgress) {
    if ((sl_sleeptimer_get_tick_count64() - t0) > limit) {
      I2C0->CMD = I2C_CMD_ABORT;              /* 버스를 놓아준다 */
      return INA_RC_TIMEOUT;
    }
    if (++guard > INA_XFER_GUARD_ITERS) {
      I2C0->CMD = I2C_CMD_ABORT;
      return INA_RC_GUARD;
    }
    rc = I2C_Transfer(I2C0);
  }
  return (int)rc;
}

/***************************************************************************//**
 * 레지스터 write — [addr+W][reg][hi][lo], 3바이트 쓰기 1회.
 ******************************************************************************/
static int ina_reg_write(uint8_t reg, uint16_t val)
{
  uint8_t tx[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };

  I2C_TransferSeq_TypeDef seq;
  seq.addr        = INA219_ADDR_SEQ;
  seq.flags       = I2C_FLAG_WRITE;
  seq.buf[0].data = tx;
  seq.buf[0].len  = sizeof(tx);
  seq.buf[1].data = NULL;
  seq.buf[1].len  = 0u;

  return i2c_xfer(&seq);
}

/***************************************************************************//**
 * 레지스터 read — [addr+W][reg] → repeated START → [addr+R][hi][lo].
 *  `I2C_FLAG_WRITE_READ` 가 repeated START 를 만들어준다. STOP 후 재시작하면
 *  멀티마스터가 아닌 환경에서도 INA219 의 포인터가 유지된다는 보장이 약해진다.
 ******************************************************************************/
static int ina_reg_read(uint8_t reg, uint16_t *val)
{
  uint8_t ptr = reg;
  uint8_t rx[2] = { 0u, 0u };

  I2C_TransferSeq_TypeDef seq;
  seq.addr        = INA219_ADDR_SEQ;
  seq.flags       = I2C_FLAG_WRITE_READ;
  seq.buf[0].data = &ptr;
  seq.buf[0].len  = 1u;
  seq.buf[1].data = rx;
  seq.buf[1].len  = sizeof(rx);

  int rc = i2c_xfer(&seq);
  if (rc == i2cTransferDone) {
    *val = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);   /* INA219 는 big-endian */
  }
  return rc;
}

/***************************************************************************//**
 * Config / Calibration 기록 + **read-back 검증**.
 *  ★write 가 ACK 됐다고 값이 들어간 게 아니다. 되읽어 일치해야 I2C 가 정말
 *   성립한 것이다 (작업지시 §4.4 `(rb)`). 일치하지 않으면 **조용히 0 을 찍는
 *   대신** 실패로 처리한다.
 ******************************************************************************/
static bool ina_configure(void)
{
  int      rc;
  uint16_t rb_cfg = 0u, rb_cal = 0u;

  rc = ina_reg_write(INA219_REG_CONFIG, INA219_CONFIG_VAL);
  if (rc != i2cTransferDone) { err_report("cfg_write", rc); return false; }

  rc = ina_reg_write(INA219_REG_CAL, INA219_CAL_VAL);
  if (rc != i2cTransferDone) { err_report("cal_write", rc); return false; }

  rc = ina_reg_read(INA219_REG_CONFIG, &rb_cfg);
  if (rc != i2cTransferDone) { err_report("cfg_rdbk", rc); return false; }

  rc = ina_reg_read(INA219_REG_CAL, &rb_cal);
  if (rc != i2cTransferDone) { err_report("cal_rdbk", rc); return false; }

  if ((rb_cfg != INA219_CONFIG_VAL) || (rb_cal != INA219_CAL_VAL)) {
    app_log("[S11a] read-back MISMATCH cfg=0x%04X(exp 0x%04X) cal=%u(exp %u)\r\n",
            (unsigned)rb_cfg, (unsigned)INA219_CONFIG_VAL,
            (unsigned)rb_cal, (unsigned)INA219_CAL_VAL);
    s_err_count++;
    return false;
  }

  app_log("[S11a] INA219 cfg=0x%04X(rb) cal=%u(rb)  addr=0x%02X OK\r\n",
          (unsigned)rb_cfg, (unsigned)rb_cal, (unsigned)INA219_ADDR_7B);
  return true;
}

/***************************************************************************//**
 * 실패 보고. **rc 를 숫자로 찍는다** — `i2cTransferNack(-1)` 인지
 * `i2cTransferBusErr(-2)` 인지가 원인 분해의 1차 분기점이기 때문이다.
 *   -1 NACK      → 주소(0x40)·A0/A1 풀다운 의심
 *   -2 BusErr    → START/STOP 오배치, 전기적 문제
 *   -3 ArbLost   → 다른 마스터? (이 보드엔 없다 → 배선/노이즈)
 *  -10 TIMEOUT   → 슬레이브 무응답 또는 클럭 미생성
 *  -11 GUARD     → ★sleeptimer 고장 의심 (1층이 안 걸렸다)
 *
 *  로그 폭주 방지: 처음 5회는 매번, 이후 60회에 1번. 에러로 UART 를 덮으면
 *  정작 봐야 할 S6a 수신 로그가 밀려난다.
 ******************************************************************************/
static void err_report(const char *step, int rc)
{
  s_err_count++;

  bool show = (s_errlog_seq < INA_ERRLOG_ALWAYS)
              || ((s_errlog_seq % INA_ERRLOG_EVERY) == 0u);
  s_errlog_seq++;

  if (show) {
    app_log("[S11a] I2C ERR step=%s rc=%d err=%lu\r\n",
            step, rc, (unsigned long)s_err_count);
  }
}

#else  /* (FS_INA >= 1) && (FS_N >= 1) */

/* ISO C 는 빈 번역 단위를 허용하지 않는다. FS_INA=0 롤백 시에도 빌드가
 * 깨지지 않도록 더미 선언을 둔다. (JUDGMENT §3.12 — 가드를 내려도 빌드는 된다) */
typedef int ina219_translation_unit_not_empty_t;

#endif /* (FS_INA >= 1) && (FS_N >= 1) */
