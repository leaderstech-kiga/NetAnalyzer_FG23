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

#if FS_N >= 6
#include "sl_rail_util_init.h"                   /* sl_rail_util_get_handle */
#include <string.h>                              /* memset */
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

#if FS_N >= 6
/* ★S6a — ISR ↔ 메인루프 인계.
 *  ISR 은 hold 만 하고 플래그를 세운다. 실제 복사/출력은 메인루프에서 한다.
 *  (ISR 에서 app_log 를 부르면 UART 링버퍼를 ISR 문맥에서 만지게 되고,
 *   슈퍼루프 규약 "Do not call blocking functions" 도 깨진다.)
 *  volatile 필수 — ISR 이 쓰고 메인루프가 읽는다. */
static volatile bool     s_s6_pending;    /* 미배출 패킷 있음 */
static volatile uint32_t s_s6_evt_count;  /* ISR 진입 횟수 (누락 진단용) */
static uint32_t          s_s6_pkt_count;  /* 메인루프가 실제 배출한 수 */

static void s6_rx_poll(void);
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

#if FS_N >= 6
  s6_rx_poll();
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

#if FS_N >= 6
/***************************************************************************//**
 * S6a — RF 수신 패킷 배출 (메인루프).
 *
 *  ★완료조건: 감지기 리셋 1회 → wake ch101 에서 LINK_PING(0x24) 1개 수신 +
 *   crc pass. (step_config.h §S6 (3) 관측 대상 표)
 *
 *  ★출력 형식 — S7 파서 전이라 **hex 덤프까지**가 목표다:
 *    [S6a] #1 ch=101 rssi=-72 crc=PASS len=8  17 02 01 01 24 05 00 01
 *                                              ^len ^VER GRP SRC MSG SEQlo SEQhi HOP
 *
 *  ★패킷 바이트 배열: RAIL 은 **wire 그대로** 준다 → buf[0]=LEN, buf[1..]=본문.
 *   (감지기 drv_rf.c:337 확인. v1.1 헤더는 buf[1] 부터 시작한다)
 *
 *  ★진단 카운터: evt(ISR 진입) vs pkt(배출) 가 어긋나면 배출이 못 따라간 것.
 ******************************************************************************/
static void s6_rx_poll(void)
{
  if (!s_s6_pending) {
    return;
  }
  s_s6_pending = false;   /* 먼저 내린다 — 배출 중 새 패킷이 오면 다시 세워짐 */

  sl_rail_handle_t rail = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);
  if (rail == NULL) {
    return;
  }

  /* hold 된 패킷이 여러 개 쌓였을 수 있다 → 없어질 때까지 배출.
   *  ⚠ 무한루프 방지 상한: 롱PA 12000bit(~4초) 라 한 번에 여러 개가 쌓일
   *    일은 거의 없지만, 상한 없이 도는 코드를 메인루프에 두지 않는다. */
  for (unsigned guard = 0; guard < 8u; guard++) {
    sl_rail_rx_packet_info_t info;
    memset(&info, 0, sizeof(info));

    sl_rail_rx_packet_handle_t h =
        sl_rail_get_rx_packet_info(rail,
                                   SL_RAIL_RX_PACKET_HANDLE_OLDEST_COMPLETE,
                                   &info);
    if (h == SL_RAIL_RX_PACKET_HANDLE_INVALID) {
      break;                        /* 더 없음 — 정상 종료 */
    }

    /* 상세정보(RSSI/CRC/채널). 실패해도 패킷은 release 해야 하므로 분기만. */
    sl_rail_rx_packet_details_t det;
    memset(&det, 0, sizeof(det));
    bool det_ok = (sl_rail_get_rx_packet_details(rail, h, &det)
                   == SL_RAIL_STATUS_NO_ERROR);

    /* 페이로드 복사. 헤더 7B + DATA 16B = 23B 가 현재 최대(STATUS_REPORT).
     *  64B 는 여유를 두되 스택을 크게 먹지 않는 선. */
    uint8_t  buf[64];
    uint16_t n = 0u;
    bool     oversize = (info.packet_bytes > (uint16_t)sizeof(buf));

    /* ⚠ sl_rail_copy_rx_packet 은 packet_bytes 전체를 p_dest 에 쓴다.
     *  버퍼보다 크면 **부분 복사가 아니라 스택 오버런**이다 → 호출하지 않는다.
     *  이 경우 길이만 보고하고 hex 는 생략한다. (현재 최대 23B 라 실제로는
     *  안 걸리지만, 깨진 프레임이 엉뚱한 LEN 을 들고 올 수 있다) */
    if (!oversize) {
      if (sl_rail_copy_rx_packet(rail, buf, &info) == SL_RAIL_STATUS_NO_ERROR) {
        n = info.packet_bytes;
      }
    }

    /* ★release 는 여기서 **반드시**. 위 분기 어디로 가도 지나간다. */
    (void)sl_rail_release_rx_packet(rail, h);

    s_s6_pkt_count++;

    /* ---- 출력 ---- */
    app_log("[S6a] #%lu ch=%u rssi=%d crc=%s len=%u%s\r\n",
            (unsigned long)s_s6_pkt_count,
            det_ok ? (unsigned)det.channel : (unsigned)FS_S6_CHANNEL,
            det_ok ? (int)det.rssi_dbm : 0,
            det_ok ? (det.crc_passed ? "PASS" : "FAIL") : "????",
            (unsigned)info.packet_bytes,
            oversize ? " (oversize -- hex 생략)" : "");

    /* hex 덤프 — 한 줄에 16B. app_log 는 가변인자 포맷이라 한 번에 길게
     *  넘기지 않고 **줄 단위**로 끊는다 (S2 에서 물린 버퍼 초과 재발 방지). */
    for (uint16_t off = 0; off < n; off += 16u) {
      char     line[16 * 3 + 1];
      unsigned pos = 0u;
      for (uint16_t i = off; (i < off + 16u) && (i < n); i++) {
        static const char hexd[] = "0123456789ABCDEF";
        line[pos++] = hexd[(buf[i] >> 4) & 0x0Fu];
        line[pos++] = hexd[buf[i] & 0x0Fu];
        line[pos++] = ' ';
      }
      line[pos] = '\0';
      app_log("[S6a]   %04u: %s\r\n", (unsigned)off, line);
    }
  }

  /* ISR 진입 수와 배출 수가 어긋나면 알린다 (조용한 유실 방지). */
  if (s_s6_evt_count != s_s6_pkt_count) {
    app_log("[S6a] note evt=%lu pkt=%lu (차이 = 미배출/중복 이벤트)\r\n",
            (unsigned long)s_s6_evt_count,
            (unsigned long)s_s6_pkt_count);
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

#if FS_N >= 6
  /* ★S6a — ISR 에서는 **hold + 플래그**만. 출력은 메인루프(s6_rx_poll).
   *  hold 를 안 하면 ISR 반환 시 RAIL 이 패킷을 자동 해제해 메인루프가
   *  꺼낼 때는 이미 없다. (감지기 drv_rf.c:361~362 과 같은 패턴)
   *  ⚠ hold 한 패킷은 반드시 release 해야 한다 — 안 하면 RX FIFO 가 차서
   *    몇 개 받고 조용히 멈춘다. release 는 s6_rx_poll 이 책임진다. */
  if (events & SL_RAIL_EVENT_RX_PACKET_RECEIVED) {
    (void)sl_rail_hold_rx_packet(rail_handle);
    s_s6_evt_count++;
    s_s6_pending = true;
  }
#endif

#if defined(SL_CATALOG_KERNEL_PRESENT)
  app_task_notify();
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------
