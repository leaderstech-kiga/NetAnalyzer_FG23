/***************************************************************************//**
 * @file
 * @brief app_process.c
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
#include "sl_component_catalog.h"
#include "sl_rail.h"
#include "sl_code_classification.h"

#if defined(SL_CATALOG_KERNEL_PRESENT)
#include "app_task_init.h"
#endif

#include "step_config.h"                    /* FS_N 스텝 가드 (JUDGMENT §3.12) */

#if FS_N >= 1
#include "app_log.h"                             /* ASCII 전용·길이 안전 로그 */
#include "sl_sleeptimer.h"
#endif

#if FS_N >= 4
#include "sl_iostream.h"
#include "sl_iostream_init_eusart_instances.h"   /* sl_iostream_Vcom_handle */
#include "uart_frame.h"
#if FS_S4 >= 2
#include "cmd_parse.h"
#endif
#endif

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------

#if FS_N >= 1
/* ★S1 완료조건: PC 터미널(115200 8-N-1)에 배너 + 1초 주기 틱이 보일 것.
 *  S0(부팅 골격)를 여기에 합쳐 판정한다 — 이 출력이 뜨면 부팅·TCXO·HFXO·EUSART가
 *  모두 살아있다는 뜻이다. (소장 결정 2026-07-30) */
#define S1_TICK_PERIOD_MS   1000u
#endif

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------

#if FS_N >= 4
static void s4a_rx_poll(void);
#endif

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------

#if FS_N >= 1
static uint64_t s_s1_next_tick;   /* 다음 틱 시각 (sleeptimer tick) */
static uint32_t s_s1_seq;         /* 틱 일련번호 — 멈춤/재부팅 육안 판별용 */
#endif

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------
/******************************************************************************
 * Application state machine, called infinitely
 *****************************************************************************/
void app_process_action(void)
{
  ///////////////////////////////////////////////////////////////////////////
  // Put your application code here!                                       //
  // This is called infinitely.                                            //
  // Do not call blocking functions from here!                             //
  ///////////////////////////////////////////////////////////////////////////

#if FS_N >= 1
  /* ★S3b: UART 링버퍼 배출. 매 루프 선두에서 호출한다(블로킹 아님).
   *  IRQ 를 못 쓰는 이유는 app_log.h 참조 — iostream autogen 이 EUSART0_TX
   *  핸들러 심볼을 이미 가져갔다. */
  app_log_tx_poll();

  /* ★S1: 1초 주기 틱 출력. 관측 수단 확보가 목적이므로 최소 구현.
   *  블로킹 delay 를 쓰지 않고 tick 비교로 돈다 — 슈퍼루프 규약("Do not call
   *  blocking functions")을 지키고, S6 이후 RF 수신과 공존할 때도 같은 패턴이
   *  그대로 쓰인다. (지금 편하자고 delay 쓰면 나중에 통째로 고쳐야 함)
   *
   *  ⚠ 단, sl_iostream_write 자체는 현재 블로킹이다. 560B 프레임(≈48.6ms)을
   *    보내는 S3 부터는 RX 를 막을 수 있어 논블로킹 TX 로 바꿔야 한다 (S3 과제). */
  uint64_t now = sl_sleeptimer_get_tick_count64();

  if (now >= s_s1_next_tick) {
    app_log("[S1] tick=%lu\r\n", (unsigned long)(++s_s1_seq));
    s_s1_next_tick = now + sl_sleeptimer_ms_to_tick(S1_TICK_PERIOD_MS);
  }
#endif

#if FS_N >= 4
  s4a_rx_poll();
#endif
}

#if FS_N >= 4
/***************************************************************************//**
 * S4a — UART 수신 폴링. **프레임 검증까지만** 하고 명령 해석은 안 한다.
 *  명령 해석(S4b/S4c)을 여기 같이 넣으면, 실패했을 때 원인이 수신인지 파서인지
 *  안 갈린다. (§3.12 한 번에 한 스텝)
 ******************************************************************************/
static void s4a_rx_poll(void)
{
  uint8_t     chunk[64];
  size_t      got = 0u;
  sl_status_t st  = sl_iostream_read(sl_iostream_Vcom_handle,
                                     chunk, sizeof(chunk), &got);

  if ((st != SL_STATUS_OK) || (got == 0u)) {
    return;                                   /* SL_STATUS_EMPTY = 수신분 없음 */
  }

  for (size_t i = 0; i < got; i++) {
    uart_frame_rx_t2       f;
    uart_frame_rx_result_t r = uart_frame_rx_feed(chunk[i], &f);

    switch (r) {
      case UART_FRAME_RX_OK:
        app_log("[S4a] rx OK   type=%c len=%03u CHK=%04X resync=%lu\r\n",
                f.type, (unsigned)f.len, f.calc_crc,
                (unsigned long)uart_frame_rx_resyncs());
#if FS_S4 >= 2
        /* ★S4b: 명령(TYPE='C') 만 해석해 ACK 회신.
         *  'R'/'A' 는 우리가 보낸 것이 되돌아온 경우라 처리하지 않는다
         *  (에코백 환경에서 무한 왕복을 만들지 않으려는 안전장치). */
        if (f.type == UART_FRAME_TYPE_CMD) {
          cmd_parse_handle(f.data, f.len);
        }
#endif
        break;

      case UART_FRAME_RX_BAD_CRC:
        app_log("[S4a] rx CRC FAIL calc=%04X recv=%04X resync=%lu\r\n",
                f.calc_crc, f.recv_crc, (unsigned long)uart_frame_rx_resyncs());
        break;

      case UART_FRAME_RX_BAD_HDR:
        /* 가짜 동기를 6B 만에 거른 경우. 정상 상황에서도 재동기 중엔 나올 수 있다. */
        app_log("[S4a] rx BAD_HDR (false sync, rejected in 6B) resync=%lu\r\n",
                (unsigned long)uart_frame_rx_resyncs());
        break;

      case UART_FRAME_RX_BAD_ED:
        app_log("[S4a] rx BAD_ED (tail is not 'ED') resync=%lu\r\n",
                (unsigned long)uart_frame_rx_resyncs());
        break;

      case UART_FRAME_RX_BAD_LEN:
        app_log("[S4a] rx BAD_LEN resync=%lu\r\n",
                (unsigned long)uart_frame_rx_resyncs());
        break;

      case UART_FRAME_RX_NONE:
      default:
        break;                                /* 아직 수집 중 */
    }
  }
}
#endif

/******************************************************************************
 * RAIL callback, called if a RAIL event occurs
 *****************************************************************************/
SL_CODE_RAM void sl_rail_util_on_event(sl_rail_handle_t rail_handle, sl_rail_events_t events)
{
  (void) rail_handle;
  (void) events;

  ///////////////////////////////////////////////////////////////////////////
  // Put your RAIL event handling here!                                    //
  // This is called from ISR context.                                      //
  // Do not call blocking functions from here!                             //
  ///////////////////////////////////////////////////////////////////////////

#if defined(SL_CATALOG_KERNEL_PRESENT)
  app_task_notify();
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------
