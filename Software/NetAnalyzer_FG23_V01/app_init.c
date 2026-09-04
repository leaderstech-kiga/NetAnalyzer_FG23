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
               "UART 보율이 115200(FG23_UART_인터페이스.md)과 다름");

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
 *   RAIL init 이상을 유발한다(V03 Step 4 회귀, 1.4mA). **조건 없이 항상 활성.** */
__attribute__((constructor))
static void rf_tcxo_power_early(void)
{
  CMU->CLKEN0_SET = CMU_CLKEN0_GPIO;        /* GPIO 버스클럭 enable */
  GPIO_PinModeSet(BOARD_TCXO_VCC_PORT, BOARD_TCXO_VCC_PIN,
                  gpioModePushPull, 1);     /* TCXO ON (PC09 HIGH) — 이후 끄지 않음 */
  for (volatile uint32_t d = 0; d < BOARD_TCXO_SETTLE_LOOPS; d++) { }  /* 기동 settle */
}

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

      /* (3) 상시 수신 시작. 스케줄러 미사용(NULL) = 계속 RX 상태 유지.
       *     S8 스캔 스케줄러 전까지는 **한 채널 고정**이 맞다 — 채널을 돌리면
       *     "안 잡힘"이 채널 탓인지 타이밍 탓인지 안 갈린다. (한 번에 한 변수) */
      sl_rail_status_t rxst = sl_rail_start_rx(rail,
                                               (uint16_t)FS_S6_CHANNEL,
                                               NULL);

      app_log("[S6a] start_rx ch=%u %s  (crc_err_show=%d)\r\n",
              (unsigned)FS_S6_CHANNEL,
              (rxst == SL_RAIL_STATUS_NO_ERROR) ? "OK" : "FAIL",
              (int)FS_S6_CRC_ERR_SHOW);

      if (rxst != SL_RAIL_STATUS_NO_ERROR) {
        /* 여기서 실패하면 수신은 시작도 안 된 것이다. 이후 "무출력" 을
         *  전파 문제로 오진하지 않도록 상태를 명시한다. */
        app_log("[S6a] FAIL rx not armed (status=0x%04X) "
                "-- 채널/PHY 설정부터 확인\r\n", (unsigned)rxst);
      } else {
        app_log("[S6a] listening... "
                "(감지기 DIP: GROUP=1 / NODE=1 확인 후 리셋)\r\n");
      }
#endif
    }
  }
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
