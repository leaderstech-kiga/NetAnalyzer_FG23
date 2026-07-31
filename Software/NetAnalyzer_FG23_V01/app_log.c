/***************************************************************************//**
 * @file    app_log.c
 * @brief   UART 출력 경로 구현 — 논블로킹 TX 링버퍼 (S3b) / 블로킹 fallback
 ******************************************************************************/

#include "app_log.h"
#include "step_config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sl_component_catalog.h"
#include "sl_iostream.h"
#include "sl_iostream_init_eusart_instances.h"   /* sl_iostream_Vcom_handle */

#if FS_TX_NONBLOCKING
#include "em_eusart.h"
#include "em_core.h"
#include "sl_iostream_eusart_vcom_config.h"      /* SL_IOSTREAM_EUSART_VCOM_PERIPHERAL */

/* ★[원본 -- 2026-07-30 1차 시도: EUSART0_TX_IRQHandler 를 우리가 정의하려 했음]
 *   근거로 "TX IRQ 는 Power Manager 조건부라 비어 있다"(sl_iostream_uart.c:240~253)
 *   를 들었으나 **틀렸다**. NVIC 활성화만 조건부이고, 핸들러 심볼은
 *   `autogen/sl_iostream_init_eusart_instances.c:192` 가 **무조건** 정의한다:
 *     void SL_IOSTREAM_EUSART_TX_IRQ_HANDLER(..._PERIPHERAL_NO)(void)
 *     { sl_iostream_eusart_irq_handler(&sl_iostream_Vcom); }
 *   → 링크 시 multiple definition 에러. (SDK 소스·템플릿만 보고 프로젝트
 *     autogen 생성물을 확인하지 않은 것이 원인)
 *
 *  ★현재 방식: 슈퍼루프 **폴링 배출**.
 *   링버퍼에 복사 후 즉시 반환한다는 핵심(= 호출자가 48.6ms 안 묶임)은 동일하고,
 *   IRQ 심볼을 건드리지 않아 iostream 과 충돌하지 않는다.
 *   배출 속도: 115200 = 1바이트당 87us. 슈퍼루프는 그보다 훨씬 빨리 도므로
 *   방문마다 FIFO 여유만큼 밀어 넣으면 회선 속도를 따라간다.
 *   루프가 잠깐 밀려도 데이터는 링에 남아 **유실되지 않는다**(지연될 뿐). */

/* ★TX 데이터 경로를 iostream 과 나눠 쓰지 않는다는 전제.
 *  Power Manager 가 설치되면 iostream 이 TXC 인터럽트로 EM 요구를 가감하는데
 *  (sl_iostream_eusart.c:566, 728), 우리가 TXDATA 를 직접 쓰면 그 장부가
 *  어긋난다. 증상은 "가끔 슬립이 안 풀린다" 같은 추적 어려운 형태다.
 *  → 조용히 깨지지 말고 **빌드가 서게** 한다. (S2 교훈: 조용한 실패가 제일 비싸다) */
#if defined(SL_CATALOG_POWER_MANAGER_PRESENT)
#error "Power Manager 가 설치되면 iostream 이 TXC 인터럽트로 EM 요구를 관리한다. \
app_log 가 TXDATA 를 직접 쓰는 것과 충돌하므로, Power Manager 를 쓰려면 \
FS_TX_NONBLOCKING=0 (블로킹 경로) 으로 되돌릴 것."
#endif

#define EUSART_TX_PERIPH   SL_IOSTREAM_EUSART_VCOM_PERIPHERAL   /* = EUSART0 */

/* 링 크기 — 2의 거듭제곱(마스킹). 560B 프레임 3개 + 로그 여유.
 * RAM 32KB 중 2KB. S6 이후 실제 프레임 발생률을 보고 조정한다(지금은 상한 추정). */
#define TX_RING_LEN   2048u
#define TX_RING_MASK  (TX_RING_LEN - 1u)

static uint8_t           s_ring[TX_RING_LEN];
static volatile uint16_t s_head;      /* 생산자(메인)만 갱신 */
static volatile uint16_t s_tail;      /* 소비자(IRQ)만 갱신 */
static volatile uint32_t s_dropped;

/* 단일 생산자(메인 루프) / 단일 소비자(IRQ) 라 락이 필요 없다:
 *  - head 는 메인만, tail 은 IRQ 만 쓴다.
 *  - 메인이 읽은 tail 이 낡았다면 여유를 **과소평가**할 뿐이라 안전하다. */
static inline uint16_t ring_used(void)
{
  return (uint16_t)((s_head - s_tail) & TX_RING_MASK);
}

/* 링에 통째로 넣는다. 여유 부족이면 아무것도 안 넣고 false. */
static bool ring_push_all(const uint8_t *src, size_t len)
{
  if (len == 0u) {
    return true;
  }
  /* 가득 참과 빔을 구분하려면 1바이트를 비워 둬야 한다 → 가용 = LEN-1-used */
  if (len > (size_t)(TX_RING_LEN - 1u - ring_used())) {
    return false;
  }

  uint16_t h = s_head;
  for (size_t i = 0; i < len; i++) {
    s_ring[h] = src[i];
    h = (uint16_t)((h + 1u) & TX_RING_MASK);
  }

  __DMB();          /* 데이터 기록이 head 갱신보다 먼저 보이도록 */
  s_head = h;

  return true;
}
#endif /* FS_TX_NONBLOCKING */

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void app_log_init(void)
{
#if FS_TX_NONBLOCKING
  s_head    = 0u;
  s_tail    = 0u;
  s_dropped = 0u;
  /* IRQ 를 쓰지 않으므로 NVIC 설정 없음 — 배출은 app_log_tx_poll() 이 한다. */
#endif
}

void app_log_tx_poll(void)
{
#if FS_TX_NONBLOCKING
  /* TX FIFO 에 여유가 있는 동안(=STATUS.TXFL) 링에서 밀어 넣는다.
   * TXFL 의미: "transmit FIFO is not full" (em_eusart.c:446 사용례). */
  while (s_tail != s_head) {
    if ((EUSART_TX_PERIPH->STATUS & EUSART_STATUS_TXFL) == 0u) {
      break;                                  /* FIFO 참 → 다음 방문에 이어서 */
    }
    EUSART_TX_PERIPH->TXDATA = (uint32_t)s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) & TX_RING_MASK);
  }
#endif
}

void app_log_raw(const char *data, size_t len)
{
  if ((data == NULL) || (len == 0u)) {
    return;
  }

#if FS_TX_NONBLOCKING
  if (!ring_push_all((const uint8_t *)data, len)) {
    s_dropped += (uint32_t)len;       /* 전부-아니면-전무 (부분 기록 금지) */
  }
#else
  /* [S3a 원본 — 블로킹 경로. FS_TX_NONBLOCKING=0 으로 즉시 복귀 가능] */
  (void)sl_iostream_write(sl_iostream_Vcom_handle, data, len);
#endif
}

void app_log(const char *fmt, ...)
{
  char    buf[APP_LOG_BUF_LEN];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n <= 0) {
    return;                                   /* 인코딩 오류 */
  }

  /* ★핵심: n 은 "필요했을 길이"라 버퍼보다 클 수 있다. 반드시 clamp. */
  size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : (sizeof(buf) - 1u);

  app_log_raw(buf, len);
}

uint32_t app_log_dropped(void)
{
#if FS_TX_NONBLOCKING
  return s_dropped;
#else
  return 0u;
#endif
}

size_t app_log_pending(void)
{
#if FS_TX_NONBLOCKING
  return (size_t)ring_used();
#else
  return 0u;
#endif
}
