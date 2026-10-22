/***************************************************************************//**
 * @file
 * @brief app_init.c
 *******************************************************************************
 * # License
 * <b>Copyright 2018 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include "em_device.h"                      /* EMU / WDOG0 레지스터 (S14e) */
#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_gpio.h"                        /* SL_GPIO_PORT_x (교차검증용) */

#include "step_config.h"                    /* FS_N 스텝 가드 (JUDGMENT §3.12) */
#include "board_cfg.h"                      /* TCXO/UART 핀 상수 + 근거 */
#include "sl_iostream_eusart_vcom_config.h" /* 핀 교차검증용 (Studio 소유) */

#if FS_N >= 1
#include "app_log.h"                             /* ASCII 전용·길이 안전 로그 */
#include "sl_sleeptimer.h"                       /* S3b 호출 반환시간 측정 */
#endif

#if FS_N >= 2
#include "uart_frame.h"                          /* CRC-16/CCITT-FALSE */
#endif

#if (FS_INA >= 1) && (FS_N >= 1)
#include "ina219.h"                              /* S11a INA219 전류 감시 */
#endif

#if FS_N >= 5
#include "sl_rail.h"
#include "sl_rail_util_init.h"                   /* sl_rail_util_get_handle */
#endif

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

/* ★Studio 설정 ↔ board_cfg.h 기대값 교차검증 (컴파일 타임).
 *  근거: 2026-07-30 프로젝트 생성 시 Studio 가 보드 기본값(PA08/PA09)으로 만들었고,
 *   파트 변경 시 config 헤더가 재생성되며 되돌아갈 수 있음을 실제로 겪었다.
 *   → 조용히 어긋나는 대신 빌드를 깨서 알아채도록 한다. (추측보다 실측/명시) */
/* ※ 포트는 sl_gpio 도메인(SL_GPIO_PORT_x)과 emlib 도메인(gpioPortx)의 열거값이
 *   같다는 보장이 없으므로 섞어 비교하지 않는다. Studio 설정끼리(sl_gpio) 비교한다. */
_Static_assert(SL_IOSTREAM_EUSART_VCOM_TX_PORT == SL_GPIO_PORT_B
               && SL_IOSTREAM_EUSART_VCOM_TX_PIN == BOARD_UART_TX_PIN,
               "UART TX 가 연결보드 V01(PB01)과 다름 - sl_iostream_eusart_vcom_config.h 확인");
_Static_assert(SL_IOSTREAM_EUSART_VCOM_RX_PORT == SL_GPIO_PORT_B
               && SL_IOSTREAM_EUSART_VCOM_RX_PIN == BOARD_UART_RX_PIN,
               "UART RX 가 연결보드 V01(PB02)과 다름 - sl_iostream_eusart_vcom_config.h 확인");
_Static_assert(SL_IOSTREAM_EUSART_VCOM_BAUDRATE == BOARD_UART_BAUDRATE,
               "UART 보율이 921600(FG23_UART_인터페이스.md §0.1)과 다름"  /* [원본 -- 115200] */);

/* ★필수 초기화: TCXO_VCC (PC09) 를 main 이전에 ON.
 *  근거: `프로젝트생성_V03_RAIL_가이드.md` §4, LTD_W10D_V03 `app_init.c` 검증 패턴.
 *   부팅 시 RAIL init(sl_rail_util_init) 이 HFXO 39MHz 를 요구한다. A3125LT 모듈은
 *   외부 TCXO(DSB211SDN)를 쓰고 그 VCC 는 MCU 의 PC09 로 게이팅된다.
 *   TCXO 가 안 켜진 채로 RAIL init 에 진입하면 RAIL_ConfigChannels assert/hang → HardFault.
 *   그래서 생성자(__attribute__((constructor)))로 main 이전에 켠다.
 *
 *  ★감지기와의 차이 (본 장비 = 비배터리 상시전원):
 *   감지기는 EM2 진입 때 이 핀을 OFF 해 1.2µA 를 달성했지만, 본 장비는 게이팅하지
 *   않고 **상시 ON** 으로 둔다. TCXO 재기동 settle(수 ms) 동안 수신을 놓치는 쪽이
 *   전류보다 훨씬 비싸다. (작업지시 §2 "고성능")
 *
 *  ★V03 교훈 계승: 이 생성자를 스텝 가드(#if)로 감싸면 특정 스텝에서 TCXO 미기동 →
 *   RAIL init 이상을 유발한다(V03 Step 4 회귀, 1.4mA). **조건 없이 항상 활성.**
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  ★S14a (2026-10-15) — `POW_EN_CON` 전원 자기유지 래치를 **여기서 가장 먼저** 올린다.
 *   근거: 작업지시_ClaudeCode_S14a_전원래치_20261014.md §3
 *
 *   **왜 생성자인가**: S2 를 떼면 홀드업 **2.2초**(C16 10uF / R39 100k, U4 EN 임계
 *   1.231V) 안에 래치를 걸어야 한다. TCXO settle(busy loop 40000) · HFXO 기동 ·
 *   RAIL init 을 기다리면 그 마진을 까먹는다. **2.2초를 쓰지 않고 남기는 것**이 목적.
 *
 *   **왜 TCXO 보다 먼저인가**: TCXO settle 루프가 수 ms 를 소비한다. 전원 래치가
 *   그 뒤에 있을 이유가 없다. 둘 다 GPIO 클럭만 있으면 되므로 순서는 자유롭고,
 *   **더 급한 쪽(전원)을 앞에** 둔다.
 *
 *   ★**#if 가드로 감싸지 않는다.** 위 V03 교훈이 TCXO 에 해당하는데, 전원 래치는
 *    그보다 더하다 — 가드에 걸리면 **장비가 아예 켜지지 않는다.**
 *    `FS_POWER` 는 S14b~d 진행을 기록하는 축일 뿐 이 코드를 가드하지 않는다.
 *
 *   [!] **LOW 로 만드는 코드는 넣지 않는다.** POW_EN_CON LOW = 2.2초 뒤 전원 차단.
 *       끄는 기능은 S14c(PWROFF). 상세는 board_cfg.h 의 BOARD_POW_EN_CON_* 주석.
 * ───────────────────────────────────────────────────────────────────────────── */
/* ★S14e — 리셋 원인 보존. 생성자 **가장 첫 줄**에서 읽는다.
 *  왜 여기인가: RSTCAUSE 는 `EMU_CMD_RSTCAUSECLR` 로 지워지는 **휘발성 증거**다.
 *   지금 빌드에는 그걸 지우는 코드(em_rmu.c)가 없지만, 컴포넌트가 하나 추가되는
 *   순간 조용히 사라질 수 있다. 생성자는 sl_main_init 보다 먼저 도므로
 *   **어떤 SDK 초기화보다도 앞**이다 — 여기서 떠 두면 그 위험이 사라진다.
 *  (.data/.bss 초기화는 __libc_init_array 보다 먼저 끝나므로 이 변수는 안전하다) */
static volatile uint32_t s_rstcause = 0u;

__attribute__((constructor))
static void rf_tcxo_power_early(void)
{
  s_rstcause = EMU->RSTCAUSE;               /* ★S14e — 지워지기 전에 먼저 떠 둔다 */

  CMU->CLKEN0_SET = CMU_CLKEN0_GPIO;        /* GPIO 버스클럭 enable */

  /* ★S14a — 전원 자기유지 래치. 클럭 enable 직후, TCXO 보다 **먼저**.
   *  push-pull HIGH = 전원 유지. D11 이 11.45V 역류를 막으므로 푸시풀로 안전.
   *  (GPIO_PinModeSet 은 out 값을 함께 받으므로 LOW 가 순간이라도 나가지 않는다) */
  GPIO_PinModeSet(BOARD_POW_EN_CON_PORT, BOARD_POW_EN_CON_PIN,
                  gpioModePushPull, 1);     /* POW_EN_CON HIGH (PC01) — 끄지 않음 */

  GPIO_PinModeSet(BOARD_TCXO_VCC_PORT, BOARD_TCXO_VCC_PIN,
                  gpioModePushPull, 1);     /* TCXO ON (PC09 HIGH) — 이후 끄지 않음 */
  for (volatile uint32_t d = 0; d < BOARD_TCXO_SETTLE_LOOPS; d++) { }  /* 기동 settle */
}

/***************************************************************************//**
 * ★S14e-1 — 리셋 원인 출력.
 *
 *  **왜 워치독과 한 쌍인가** (소장 지시 2026-10-15):
 *   S14a 래치가 걸린 뒤 POW_EN 은 2.97V 다. 리셋이 나면 GPIO 가 high-Z 가 되어
 *   C16 이 R39 로 방전하고 **0.88초**(=1.0s·ln(2.97/1.231)) 안에 재부팅해 다시
 *   래치를 걸어야 전원이 유지된다. 워치독 리셋이 그 기한을 못 넘기면 **장비가
 *   꺼진다.** 그런데 꺼진 장비는 아무 말도 안 하므로, 다음 기동 때 이 한 줄이
 *   없으면 "워치독이 돌았다"와 "그냥 안 켰다"가 구분되지 않는다.
 *   → 대안을 만들려면 먼저 **무엇이 일어났는지**를 남겨야 한다. 그게 이 함수다.
 *
 *  ★BOD 를 따로 표시하는 이유: 브라운아웃 리셋은 "전원 레일이 내려앉았다"는
 *   뜻이고, 이 장비에서는 곧 **홀드업을 깎아먹는 중**이라는 신호다.
 *   워치독·락업과 원인도 대책도 전혀 다르므로 뭉뚱그리면 안 된다.
 *
 *  ※ FS_WDOG 로 가드하지 않는다 — 워치독을 끈 뒤에도 리셋 원인은 알아야 한다.
 *  ※ 단 FS_N>=1 가드는 필요하다 — 출력 수단(app_log)이 S1 에서 생기기 때문이다.
 *    bisection 으로 FS_N 을 0 까지 내려도 빌드가 깨지지 않아야 한다 (§3.12).
 ******************************************************************************/
#if FS_N >= 1
static void rstcause_report(void)
{
  uint32_t rc  = s_rstcause;
  uint32_t bod = rc & (EMU_RSTCAUSE_DVDDBOD   | EMU_RSTCAUSE_DVDDLEBOD
                       | EMU_RSTCAUSE_DECBOD  | EMU_RSTCAUSE_AVDDBOD
                       | EMU_RSTCAUSE_IOVDD0BOD);

  app_log("[S14e] RSTCAUSE=0x%08lX%s%s%s%s%s%s\r\n",
          (unsigned long)rc,
          (rc & EMU_RSTCAUSE_POR)    ? " POR"           : "",
          (rc & EMU_RSTCAUSE_PIN)    ? " PIN"           : "",
          (rc & EMU_RSTCAUSE_SYSREQ) ? " SYSREQ"        : "",
          (rc & EMU_RSTCAUSE_EM4)    ? " EM4"           : "",
          (rc & EMU_RSTCAUSE_WDOG0)  ? " ***WDOG***"    : "",
          (rc & EMU_RSTCAUSE_LOCKUP) ? " ***LOCKUP***"  : "");

  if (bod != 0u) {
    /* 전원 레일 이상. 홀드업(0.88초)을 깎는 요인이므로 눈에 띄게 따로 찍는다. */
    app_log("[S14e] ***BOD*** 0x%08lX -- 전원 레일 내려앉음. "
            "POW_EN/+3.3V 측정할 것\r\n", (unsigned long)bod);
  }

  /* 해석 안내 — 로그만 받아 보는 사람이 바로 판단할 수 있게 한 줄 붙인다. */
  if ((rc & EMU_RSTCAUSE_POR) != 0u) {
    app_log("       POR = 전원 투입. 정상 기동.\r\n");
  } else if ((rc & EMU_RSTCAUSE_WDOG0) != 0u) {
    app_log("       ★WDOG = 메인루프가 %u.%02us 이상 멈췄다. "
            "전원이 유지된 채 이 줄이 보이면 리셋 내성 통과.\r\n",
            (unsigned)(S14E_WDOG_PERIOD_MS / 1000u),
            (unsigned)((S14E_WDOG_PERIOD_MS % 1000u) / 10u));
  }
}
#endif /* FS_N >= 1 */

#if (FS_WDOG >= 1) && (FS_N >= 1)
/***************************************************************************//**
 * ★S14e-2 — 워치독 기동. **초기화가 전부 끝난 뒤** 호출한다.
 *
 *  **왜 레지스터 직접 접근인가**: `em_wdog.o` 가 이 프로젝트 빌드에 없다
 *   (2026-10-15 확인 — `GNU ARM v12.2.1 - Default/.../emlib/src/` 에 em_wdog.o 부재).
 *   emlib 를 쓰려면 .slcp 에 컴포넌트를 추가해야 하는데, 그러면 Studio 가
 *   autogen 을 다시 뱉으면서 **UART 핀·보율 설정까지 재생성**할 수 있다.
 *   그 사고는 이미 한 번 겪었다(app_init.c 상단 _Static_assert 의 유래).
 *   워치독 하나 켜자고 그 위험을 지지 않는다. 레지스터 5줄이면 끝난다.
 *
 *  ★★기능클럭을 **반드시 명시**한다 — 이게 이 함수의 유일한 함정이다.
 *   `CMU_WDOG0CLKCTRL` 의 리셋 기본값은 **LFRCO(32.768kHz)** 다
 *   (efr32fg23_cmu.h:993). 안 바꾸면 2049clk 이 2.05초가 아니라 **62ms** 가 되어
 *   부팅하자마자 리셋 루프에 빠진다. 더 나쁜 건, S14a 래치가 걸린 상태라
 *   리셋 루프가 홀드업(0.88초)을 계속 깎아 **장비가 아예 꺼져버린다** —
 *   즉 "왜 안 켜지지"로 보이고 로그조차 안 나온다.
 *   ULFRCO 는 항상 돌고 별도 기동이 필요 없어 워치독 소스로 적합하다.
 *
 *  **왜 init 이 끝난 뒤인가**: sl_main_init 은 RAIL init·HFXO 기동을 포함하는데
 *   그 소요시간을 아직 **안 재봤다**. 모르는 구간을 워치독으로 덮으면 오경보가
 *   난다. 먼저 재고(아래 pre_init 출력) 그 다음에 앞당길지 정한다. (추측보다 실측)
 *
 *  설정: CFG 는 PERSEL 만 쓴다. 나머지는 리셋 기본값이 그대로 맞다 —
 *    DEBUGRUN=0 (디버거 정지 중 물지 않음. 브레이크포인트마다 리셋되면 못 쓴다)
 *    EM2RUN/EM3RUN=0 (본 프로젝트는 Power Manager 미탑재라 EM0/EM1 만 돈다)
 *    CLRSRC=0 (SW 급이), WARNSEL=0 (경고 IRQ 미사용 — 행 상태에서는 출력도 못 한다)
 *  LOCK 은 걸지 않는다. 잠그면 FS_WDOG=0 롤백이 런타임에 불가능해진다.
 ******************************************************************************/
static void wdog_start(void)
{
  CMU->CLKEN0_SET   = CMU_CLKEN0_WDOG0;                   /* 버스클럭 */
  CMU->WDOG0CLKCTRL = CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO;     /* ★기능클럭 1kHz */

  /* CFG 는 EN=0 일 때만 쓸 수 있다. 리셋 직후엔 0 이지만 명시적으로 내린다
   *  (SWD 리셋 등으로 EN 이 남아 있는 경우를 배제). em_wdog.c WDOGn_Init 과 동일 순서. */
  if (WDOG0->EN != 0u) {
    while (WDOG0->SYNCBUSY != 0u) { }
    WDOG0->EN_CLR = WDOG_EN_EN;
  }

  WDOG0->CFG    = (uint32_t)FS_WDOG_PERSEL << _WDOG_CFG_PERSEL_SHIFT;
  WDOG0->EN_SET = WDOG_EN_EN;

  app_log("[S14e] WDOG on  ULFRCO persel=%d (~%u ms)  feed=main loop\r\n",
          (int)FS_WDOG_PERSEL, (unsigned)S14E_WDOG_PERIOD_MS);
}
#endif /* FS_WDOG */

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------
/******************************************************************************
 * The function is used for some basic initialization related to the app.
 *****************************************************************************/
void rail_app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your application init code here!                                    //
  // This is called once during start-up.                                    //
  /////////////////////////////////////////////////////////////////////////////

#if FS_N >= 1
  /* ★S14e — 여기 도달한 시각 = sl_main_init(RAIL init·HFXO 기동 포함) 소요시간의
   *  하한. sleeptimer 가 자기 자신을 초기화한 시점부터 세므로 **정확한 부팅시간이
   *  아니라 하한**이다. 그래도 "워치독을 init 구간까지 앞당겨도 되는가"를 판단할
   *  유일한 실측 근거다. 이 값이 주기(2.05초)에 비해 충분히 작으면 앞당길 수 있다.
   *  ※ 값 자체가 목적이므로 가드 없이 항상 잰다 (호출 1회, 비용 무시 가능). */
  uint32_t pre_init_ms = (uint32_t)sl_sleeptimer_tick_to_ms(
                             (uint32_t)sl_sleeptimer_get_tick_count64());
#endif

#if FS_N >= 1
  /* ★출력 경로 초기화 — 반드시 첫 출력보다 먼저 (S3b 링버퍼/TX IRQ 준비) */
  app_log_init();

  /* ★S1: 부팅 배너. 이 줄이 PC 터미널에 뜨면 부팅·TCXO·HFXO·EUSART 가 전부 정상.
   *  (S0 를 S1 에 합쳐 판정 — 소장 결정 2026-07-30)
   *  틱과 달리 배너는 부팅 1회만 나오므로, 터미널에 배너가 반복되면 = 리셋 반복. */
  /* ★보율을 하드코딩하지 않는다 (2026-07-31 정정).
   *  115200 → 921600 으로 바꿨을 때 이 문자열만 115200 으로 남아 **거짓 정보**를
   *  출력했다. 로그가 설정과 어긋나면 나중에 그 로그를 근거로 디버깅하다 헤맨다.
   *  (같은 실수: S3b 의 "RING+IRQ" 라벨)
   *  [원본 -- "UART 115200 8-N-1 / PB01=TX PB02=RX"] */
  app_log("\r\n=== NetAnalyzer_FG23_V01 (FS_N=%d) ===\r\n"
          "UART %lu 8-N-1 / PB%02u=TX PB%02u=RX\r\n",
          FS_N, (unsigned long)BOARD_UART_BAUDRATE,
          (unsigned)BOARD_UART_TX_PIN, (unsigned)BOARD_UART_RX_PIN);

  /* ★S14e — 배너 **바로 다음**에 찍는다. 배너가 반복되면 리셋 반복이라는 건
   *  이미 알고 있었지만(위 주석), 그때 "왜" 를 알 수 없었다. 이 줄이 그 답이다. */
  rstcause_report();
  app_log("[S14e] pre_init=%lu ms (sl_main_init 소요 하한)\r\n",
          (unsigned long)pre_init_ms);
#endif

#if FS_N >= 2
  /* ★S2 완료조건: CRC-16/CCITT-FALSE 표준 벡터 2개 통과 (FG23_UART §1.2).
   *  FAIL 시 **실제 계산값을 같이 찍는다** — "FAIL" 만 나오면 진단이 안 되고,
   *  값을 보면 어디가 틀렸는지(init/refin/xorout 등) 바로 좁혀진다.
   *  ※ 출력은 ASCII 전용 (app_log.h 참조 — 한글 UTF-8 이 길이 계산을 깨뜨렸던 건). */
  {
    uint16_t c1 = 0u, c2 = 0u;
    bool ok = uart_frame_crc_selftest(&c1, &c2);

    app_log("[S2] CRC16/CCITT-FALSE %s\r\n", ok ? "PASS" : "FAIL");
    app_log("     \"123456789\" = %04X (exp %04X)\r\n",
            c1, UART_FRAME_CRC_VECTOR_123456789);
    app_log("     \"ABC\"       = %04X (exp %04X)\r\n",
            c2, UART_FRAME_CRC_VECTOR_ABC);
  }
#endif

#if FS_N >= 3
  /* ★S3a 완료조건: 스펙 §1.2 "검증 예제" 프레임 재현 — CHK==EB7D, 길이 560.
   *  이게 맞으면 필드 폭·패딩·LEN 계산·CRC 범위가 전부 스펙과 일치한다는 뜻.
   *  프레임 원문도 같이 흘려 육안 판독 가능하게 한다(전부 printable ASCII, §1). */
  {
    uint16_t    crc   = 0u;
    size_t      len   = 0u;
    const char *frame = NULL;
    bool ok = uart_frame_build_selftest(&crc, &len, &frame);

    app_log("[S3a] frame build %s  len=%u (exp %u)  CHK=%04X (exp %04X)\r\n",
            ok ? "PASS" : "FAIL",
            (unsigned)len, (unsigned)UART_FRAME_TOTAL_LEN,
            crc, UART_FRAME_EXAMPLE_CRC);

    if (frame != NULL && len > 0u) {
      app_log("[S3a] frame>>\r\n");

      /* ★S3b 완료조건: 560B 송신 **호출 반환 시간** 측정.
       *  블로킹(FS_TX_NONBLOCKING=0) = 560B@115200 ≈ 48.6ms 를 여기서 다 쓴다.
       *  논블로킹(=1) = 링에 memcpy 만 하고 즉시 반환 → 1ms 미만이어야 한다.
       *  ※ 측정 대상은 "전송 완료"가 아니라 "호출이 돌아오는 시간"이다.
       *    전송 자체는 어느 쪽이든 48.6ms 가 걸린다(회선 속도) — 요점은
       *    그동안 CPU 가 묶이느냐다. */
      uint32_t t0 = (uint32_t)sl_sleeptimer_get_tick_count64();
      app_log_raw(frame, len);            /* 560B — app_log 버퍼로는 못 보냄 */
      uint32_t t1 = (uint32_t)sl_sleeptimer_get_tick_count64();

      app_log("\r\n[S3a] <<end\r\n");

      uint32_t f  = sl_sleeptimer_get_timer_frequency();
      uint32_t dt = t1 - t0;
      uint32_t us = (f != 0u) ? (uint32_t)(((uint64_t)dt * 1000000u) / f) : 0u;

      app_log("[S3b] tx_mode=%s  call_return=%lu tick (%lu us)  limit<1000us\r\n",
              /* [원본 -- "RING+IRQ" : IRQ 방식 1차 시도 때의 라벨. 실제 구현은
               *  슈퍼루프 폴링 배출이라 오해 소지가 있어 정정 (app_log.h 참조)] */
              FS_TX_NONBLOCKING ? "RING+POLL" : "BLOCKING",
              (unsigned long)dt, (unsigned long)us);
      app_log("[S3b] tick_freq=%lu Hz  pending=%u  dropped=%lu\r\n",
              (unsigned long)f, (unsigned)app_log_pending(),
              (unsigned long)app_log_dropped());
    }
  }
#endif

#if FS_N >= 5
  /* ★S5 완료조건: RAIL 핸들 획득 + 채널 유효성.
   *  radioconf 2그룹이 실제로 rail_config 에 반영됐는지를 **런타임에서** 본다.
   *  빌드 시점 확인(rail_config.c 육안 대조)은 §1 에서 이미 했으나, 그건
   *  "생성물이 맞다"까지고 "칩이 그렇게 동작한다"는 아니다.
   *
   *  ★여기까지 왔다는 것 자체가 TCXO/HFXO 검증이기도 하다 —
   *   sl_rail_util_init(sl_event_handler.c:50)이 39MHz HFXO 를 요구하므로,
   *   TCXO 생성자가 실패했으면 이 줄에 도달하기 전에 assert/hang 이 난다.
   *
   *  검사 대상: 그룹 경계 + 사이 구간.
   *   0,24    = 데이터 그룹(짧은PA) 양끝     → 유효 기대
   *   100,124 = wake 그룹(롱PA) 양끝         → 유효 기대
   *   25,50,99,125 = 그룹 사이/밖            → **무효 기대**
   *  마지막 줄이 중요하다. cmd_parse 의 ch_is_valid() 가 "25~99 는 없는 채널"
   *  이라고 전제하는데, 그 전제를 라디오에게 직접 확인받는 것이다. */
  {
    sl_rail_handle_t rail = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);

    if (rail == NULL) {
      app_log("[S5] FAIL rail handle is NULL\r\n");
    } else {
      static const uint16_t probe[]  = { 0u, 24u, 100u, 124u, 25u, 50u, 99u, 125u };
      static const bool     expect[] = { true, true, true, true, false, false, false, false };
      unsigned pass = 0u;

      app_log("[S5] rail handle OK\r\n");
      for (unsigned i = 0; i < (sizeof(probe) / sizeof(probe[0])); i++) {
        bool valid = (sl_rail_is_valid_channel(rail, probe[i])
                      == SL_RAIL_STATUS_NO_ERROR);
        bool ok    = (valid == expect[i]);
        pass += ok;
        app_log("[S5] ch=%03u %-7s (exp %-7s) %s\r\n",
                (unsigned)probe[i],
                valid     ? "valid" : "invalid",
                expect[i] ? "valid" : "invalid",
                ok ? "PASS" : "FAIL");
      }
      app_log("[S5] result: %u/%u PASS\r\n",
              pass, (unsigned)(sizeof(probe) / sizeof(probe[0])));

#if FS_N >= 6
      /* ★S6a — RF 수신 무장. S5 로 핸들·채널이 확인된 **직후**에만 한다.
       *
       *  ★안전 (절대선 / JUDGMENT §1):
       *   여기는 **수신 전용**이다. 송신 API(sl_rail_start_tx 계열)는 한 줄도
       *   없고, FS_N<9 TX 컴파일 제외 정책도 그대로다. 스니퍼가 감지기의
       *   경보·연동에 개입하는 경로가 생기지 않는다. (소장 결정 2026-09-02
       *   옵션 A = 수동 관측, QUERY TX 앞당기지 않음)
       *
       *  ★감지기 참고 구현: LTD_W10D_V03/drv_rf.c:95(config_events),
       *   :290(start_rx), :354~371(on_event). **로직만** 가져왔다 —
       *   API 세대가 다르다:
       *     감지기 = RAIL_Handle_t / RAIL_StartRx / RAIL_Events_t   (구)
       *     스니퍼 = sl_rail_handle_t / sl_rail_start_rx / sl_rail_events_t (신)
       *   심볼은 추측하지 않고 링커맵(NetAnalyzer_FG23_V01.map)에 실제로
       *   링크된 이름과 SDK 헤더 시그니처로 확인했다.
       *   (simplicity_sdk_2/platform/radio/rail_lib/common/sl_rail.h:3525 등) */

      /* (1) RX_PACKET_RECEIVED 이벤트만 명시 활성.
       *     mask = 건드릴 비트, events = 그 비트에 넣을 값. 다른 이벤트는
       *     rail_util_init 컴포넌트 설정을 그대로 둔다. */
      (void)sl_rail_config_events(rail,
                                  SL_RAIL_EVENT_RX_PACKET_RECEIVED,
                                  SL_RAIL_EVENT_RX_PACKET_RECEIVED);

      /* (2) CRC 깨진 패킷 처리 방침 (step_config.h FS_S6_CRC_ERR_SHOW 참조).
       *     RAIL 기본은 CRC 오류 패킷을 조용히 버린다 → 스니퍼로는 최악이다.
       *     "안 옴" 과 "왔는데 깨짐" 이 구분돼야 원인을 좁힐 수 있다. */
#if FS_S6_CRC_ERR_SHOW
      (void)sl_rail_config_rx_options(rail,
                                      SL_RAIL_RX_OPTION_IGNORE_CRC_ERRORS,
                                      SL_RAIL_RX_OPTION_IGNORE_CRC_ERRORS);
#endif

      /* (3) ★★RX 자동 재무장 (2026-09-04 추가 — S6a 실측 결함 수정).
       *
       *  [결함] 초판(앵커 S6a_20260904.s37)에는 이 블록이 없었다.
       *   `config/sl_rail_util_init_inst0_config.h:161,:166` 이
       *     SL_RAIL_UTIL_INIT_TRANSITION_INST0_RX_SUCCESS  RAIL_RF_STATE_IDLE
       *     SL_RAIL_UTIL_INIT_TRANSITION_INST0_RX_ERROR    RAIL_RF_STATE_IDLE
       *   이라 **패킷 1개를 받으면 라디오가 IDLE 로 떨어진다.** 재무장이 없으니
       *   그 뒤로는 영원히 못 받는다.
       *   실측 증상: 화재 10회 발생 중 **1회만 수신**. 전파 문제로 오해하기 쉽다
       *   (RSSI 가 -6dBm 로 아주 강했는데도 안 들어왔다).
       *
       *  [왜 놓쳤나] 감지기는 이 함정을 이미 겪고 기록해 뒀다 —
       *   LTD_W10D_V03/drv_rf.c:158~161 "명시적 재활성 없으면 이후 알람 수신 불가".
       *   참고 구현에서 **재무장 부분만 빠뜨렸다.** 메모 §5 의
       *   "생성물(autogen)까지 봐야 확인이다" 가 형태를 바꿔 재발한 것 —
       *   이번엔 rail_util_init **설정 헤더**를 안 봤다. RAIL 은 코드뿐 아니라
       *   Studio 설정이 동작을 바꾸므로 설정 헤더도 확인 대상이다.
       *
       *  [수정] 이중으로 건다. 스니퍼는 놓치면 안 되는 장비다.
       *   (a) 자동 전이를 RX 유지로 → 하드웨어가 알아서 복귀, 패킷 사이 공백 0
       *   (b) 배출 후 start_rx 재호출 (app_process.c s6_rx_poll) → (a) 실패 대비
       *  Studio config 를 직접 고치는 방법도 있으나, Studio 재생성이 설정을
       *  되돌린 사고가 이미 있어(board_cfg.h:38) **코드에서 명시**하는 쪽이 안전하다.
       *  API: sl_rail.h:2390 / sl_rail_types.h sl_rail_state_transitions_t */
      {
        sl_rail_state_transitions_t rxtr = {
          .success = SL_RAIL_RF_STATE_RX,
          .error   = SL_RAIL_RF_STATE_RX,
        };
        sl_rail_status_t trst = sl_rail_set_rx_transitions(rail, &rxtr);

        app_log("[" S6_TAG "] rx_transitions success=RX error=RX %s\r\n",
                (trst == SL_RAIL_STATUS_NO_ERROR) ? "OK" : "FAIL");

        if (trst != SL_RAIL_STATUS_NO_ERROR) {
          /* (a) 실패해도 (b) fallback 이 있으므로 치명적이지 않다.
           *  다만 연속 수신 성능이 떨어지므로 명시해 둔다. */
          app_log("[" S6_TAG "] warn: 자동 재무장 실패 -- poll 재무장(fallback)에 의존\r\n");
        }
      }

      /* (4) 상시 수신 시작. 스케줄러 미사용(NULL) = 계속 RX 상태 유지.
       *     S8 스캔 스케줄러 전까지는 **한 채널 고정**이 맞다 — 채널을 돌리면
       *     "안 잡힘"이 채널 탓인지 타이밍 탓인지 안 갈린다. (한 번에 한 변수) */
      sl_rail_status_t rxst = sl_rail_start_rx(rail,
                                               (uint16_t)FS_S6_CHANNEL,
                                               NULL);

      app_log("[" S6_TAG "] start_rx ch=%u %s  (crc_err_show=%d)\r\n",
              (unsigned)FS_S6_CHANNEL,
              (rxst == SL_RAIL_STATUS_NO_ERROR) ? "OK" : "FAIL",
              (int)FS_S6_CRC_ERR_SHOW);

      if (rxst != SL_RAIL_STATUS_NO_ERROR) {
        /* 여기서 실패하면 수신은 시작도 안 된 것이다. 이후 "무출력" 을
         *  전파 문제로 오진하지 않도록 상태를 명시한다. */
        app_log("[" S6_TAG "] FAIL rx not armed (status=0x%04X) "
                "-- 채널/PHY 설정부터 확인\r\n", (unsigned)rxst);
      } else {
        app_log("[" S6_TAG "] listening... "
                "(감지기 DIP: GROUP=1 / NODE=1 확인 후 리셋)\r\n");
      }
#endif
    }
  }
#endif

#if (FS_INA >= 1) && (FS_N >= 1)
  /* ★S11a — 워치독보다 **앞**에 둔다.
   *  초기화 자체는 1.7ms 규모라 뒤에 둬도 되지만, 버스가 물려 복구가 길어져도
   *  **워치독이 아직 안 켜진 구간**이면 부트 루프가 생기지 않는다.
   *  (S14a 래치가 걸린 상태의 부트 루프 = 홀드업을 깎아 장비가 조용히 꺼진다) */
  ina219_init();
#endif

#if (FS_WDOG >= 1) && (FS_N >= 1)
  /* ★S14e — **맨 마지막**. 위의 모든 초기화(RAIL 포함)가 끝난 뒤에만 문다.
   *  여기보다 앞에 두면 아직 안 재본 구간을 덮게 된다 (wdog_start 주석 참조). */
  wdog_start();
#endif
}

void app_init(void)
{
#if !defined(SL_CATALOG_KERNEL_PRESENT)
  rail_app_init();
#else
  app_task_init();
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------
