/***************************************************************************//**
 * @file    uart_frame.c
 * @brief   FG23 ↔ M1S UART 프레임 구현 (스펙: FG23_UART_인터페이스.md)
 ******************************************************************************/

#include "uart_frame.h"

#if FS_N >= 2

#include <string.h>

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

/* CRC-16/CCITT-FALSE — `FG23_UART_인터페이스.md` §1.2 의 C 샘플을 그대로 이식.
 *
 *  ★비트와이즈 그대로 두는 이유: 테이블 방식이 빠르지만 512B 룩업테이블이 늘고,
 *   115200 UART(560B ≈ 48.6ms) 대비 CRC 계산(552B × 8회전)은 39MHz 에서 무시 가능하다.
 *   스펙 문서의 코드와 **한 글자도 다르지 않게** 두는 편이 M1S 앱과의 호환 검증에 유리.
 *   (속도가 문제가 되면 그때 테이블화 — 지금은 추측 최적화 안 함) */
uint16_t uart_frame_crc16(const uint8_t *data, size_t len)
{
  uint16_t crc = 0xFFFFu;

  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;                     /* xorout = 0 */
}

bool uart_frame_crc_selftest(uint16_t *out_123456789, uint16_t *out_abc)
{
  static const char v1[] = "123456789";
  static const char v2[] = "ABC";

  uint16_t c1 = uart_frame_crc16((const uint8_t *)v1, sizeof(v1) - 1u);  /* 9B */
  uint16_t c2 = uart_frame_crc16((const uint8_t *)v2, sizeof(v2) - 1u);  /* 3B */

  if (out_123456789 != NULL) { *out_123456789 = c1; }
  if (out_abc       != NULL) { *out_abc       = c2; }

  return (c1 == UART_FRAME_CRC_VECTOR_123456789)
      && (c2 == UART_FRAME_CRC_VECTOR_ABC);
}

#if FS_N >= 3

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------

/* 프레임 조립 버퍼. 560B 를 스택에 두면 SL_STACK_SIZE(2048) 를 크게 잠식하고,
 * S6 이후 RAIL 콜백 경로와 겹칠 때 위험하다 → 정적 버퍼 1개를 재사용한다.
 * (동시에 2프레임을 만들지 않는다는 전제 — 슈퍼루프 단일 흐름이라 성립) */
static char s_frame[UART_FRAME_TOTAL_LEN];

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static const char HEX_UC[] = "0123456789ABCDEF";

/* 고정폭 좌측정렬 + 공백 패딩. src 가 width 보다 길면 절단(프레임 폭은 불변). */
static char *put_padded(char *p, const char *src, size_t width)
{
  size_t i = 0;
  while (i < width && src[i] != '\0') { *p++ = src[i]; i++; }
  while (i < width)                   { *p++ = ' ';    i++; }
  return p;
}

/* 부호 없는 십진, 0-pad 고정폭 (자리 초과 시 하위 자리만 남음) */
static char *put_u_dec(char *p, uint32_t v, size_t width)
{
  for (size_t i = width; i > 0; i--) { p[i - 1u] = (char)('0' + (v % 10u)); v /= 10u; }
  return p + width;
}

/* 대문자 hex, 0-pad 고정폭 */
static char *put_hex(char *p, uint32_t v, size_t width)
{
  for (size_t i = width; i > 0; i--) { p[i - 1u] = HEX_UC[v & 0xFu]; v >>= 4; }
  return p + width;
}

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

size_t uart_frame_build_rx(char *out, size_t out_len, const uart_frame_rx_t *rx)
{
  if ((out == NULL) || (rx == NULL) || (out_len < UART_FRAME_TOTAL_LEN)) {
    return 0u;
  }

  uint16_t plen = rx->payload_len;
  if (rx->payload == NULL)              { plen = 0u; }
  if (plen > UART_FRAME_PAYLOAD_MAX)    { plen = UART_FRAME_PAYLOAD_MAX; }

  char *p = out;

  /* --- ST ----------------------------------------------------------------- */
  *p++ = 'S'; *p++ = 'T';

  char *len_field = p;            /* LEN(3) 은 DATA 를 만든 뒤 되돌아와 채운다 */
  p += 3;

  /* --- TYPE --------------------------------------------------------------- */
  *p++ = UART_FRAME_TYPE_RX;

  /* --- DATA(540) : CSV 13필드 (§1.1 고정 자리수) --------------------------- */
  char *data = p;

  p = put_u_dec(p, rx->ts_ms, 10);                    /* 1 ts_ms   10 십진 */
  *p++ = ',';
  p = put_u_dec(p, rx->ch, 3);                        /* 2 ch       3 십진 */
  *p++ = ',';
  {                                                   /* 3 rssi     4 부호+3자리 */
    int32_t  v = rx->rssi_dbm;
    *p++ = (v < 0) ? '-' : '+';
    p = put_u_dec(p, (uint32_t)((v < 0) ? -v : v), 3);
  }
  *p++ = ',';
  p = put_u_dec(p, rx->lqi, 3);                       /* 4 lqi      3 십진 */
  *p++ = ',';
  /* 5 crc 4자 좌측정렬 — ★유효분(LEN)에도 이 4자가 그대로 들어간다(§1.2 예제) */
  p = put_padded(p, rx->crc_ok ? "OK" : "FAIL", 4);
  *p++ = ',';
  p = put_hex(p, rx->ver, 2);                         /* 6  ver      2 hex */
  *p++ = ',';
  p = put_hex(p, rx->group, 2);                       /* 7  group    2 hex */
  *p++ = ',';
  p = put_hex(p, rx->src, 2);                         /* 8  src      2 hex */
  *p++ = ',';
  p = put_hex(p, rx->msg_type, 2);                    /* 9  msg_type 2 hex */
  *p++ = ',';
  p = put_hex(p, rx->seq, 4);                         /* 10 seq      4 hex */
  *p++ = ',';
  p = put_hex(p, rx->hop, 2);                         /* 11 hop      2 hex */
  *p++ = ',';
  p = put_u_dec(p, plen, 3);                          /* 12 len      3 십진 */
  *p++ = ',';

  /* 여기까지가 §1.1 의 "앞 12필드 + 콤마12" = 53자 */
  if ((size_t)(p - data) != UART_FRAME_CSV_PREFIX_LEN) {
    return 0u;                                        /* 폭 계산이 어긋남 = 버그 */
  }

  /* 13 payload_hex : 486자, hex 대문자 + 우측 공백패딩 */
  char *pay = p;
  for (uint16_t i = 0; i < plen; i++) {
    *p++ = HEX_UC[(rx->payload[i] >> 4) & 0xFu];
    *p++ = HEX_UC[rx->payload[i] & 0xFu];
  }
  while ((size_t)(p - pay) < UART_FRAME_PAYLOAD_HEX_LEN) { *p++ = ' '; }

  /* LEN = 유효 길이 = 53 + payload_hex 실제분 (패딩 제외) */
  uint32_t valid_len = UART_FRAME_CSV_PREFIX_LEN + (uint32_t)plen * 2u;
  (void)put_u_dec(len_field, valid_len, 3);

  /* DATA 를 540 까지 공백 패딩 (13필드 합 539 + 1) */
  while ((size_t)(p - data) < UART_FRAME_DATA_LEN) { *p++ = ' '; }

  /* --- DUMMY(8) ----------------------------------------------------------- */
  for (size_t i = 0; i < UART_FRAME_DUMMY_LEN; i++) { *p++ = ' '; }

  /* --- CHK(4) : CRC-16/CCITT-FALSE over LEN+TYPE+DATA+DUMMY (552B) -------- */
  uint16_t crc = uart_frame_crc16((const uint8_t *)len_field,
                                  UART_FRAME_CRC_SCOPE_LEN);
  p = put_hex(p, crc, 4);

  /* --- ED ----------------------------------------------------------------- */
  *p++ = 'E'; *p++ = 'D';

  return (size_t)(p - out);
}

bool uart_frame_build_selftest(uint16_t *out_crc, size_t *out_len,
                               const char **out_frame)
{
  /* `FG23_UART_인터페이스.md §1.2 검증 예제` 의 값 그대로.
   *   CSV = 0000128374,003,-071,210,OK  ,02,01,05,01,0123,04,008,0102030405060708
   *   LEN = 069 / TYPE = R / DUMMY = 공백8 → CHK = EB7D */
  static const uint8_t payload[8] = { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08 };

  const uart_frame_rx_t rx = {
    .ts_ms       = 128374u,
    .ch          = 3u,
    .rssi_dbm    = -71,
    .lqi         = 210u,
    .crc_ok      = true,
    .ver         = 0x02u,
    .group       = 0x01u,
    .src         = 0x05u,
    .msg_type    = 0x01u,
    .seq         = 0x0123u,
    .hop         = 0x04u,
    .payload     = payload,
    .payload_len = (uint16_t)sizeof(payload),
  };

  size_t n = uart_frame_build_rx(s_frame, sizeof(s_frame), &rx);

  uint16_t crc = 0u;
  if (n == UART_FRAME_TOTAL_LEN) {
    /* CHK 필드(끝에서 6번째부터 4자)를 되읽어 검증 — 조립 결과 자체를 본다 */
    const char *chk = &s_frame[UART_FRAME_TOTAL_LEN - 6u];
    for (int i = 0; i < 4; i++) {
      char ch = chk[i];
      uint16_t d = (ch >= 'A') ? (uint16_t)(ch - 'A' + 10) : (uint16_t)(ch - '0');
      crc = (uint16_t)((crc << 4) | d);
    }
  }

  if (out_crc   != NULL) { *out_crc   = crc; }
  if (out_len   != NULL) { *out_len   = n; }
  if (out_frame != NULL) { *out_frame = s_frame; }

  return (n == UART_FRAME_TOTAL_LEN) && (crc == UART_FRAME_EXAMPLE_CRC);
}

#endif /* FS_N >= 3 */

#if FS_N >= 4

// -----------------------------------------------------------------------------
//                          S4a — 프레임 수신 상태기
// -----------------------------------------------------------------------------

typedef enum { ST_WAIT_S = 0, ST_WAIT_T, ST_COLLECT } rx_state_t;

static uint8_t    s_rx[UART_FRAME_TOTAL_LEN];
static uint16_t   s_rx_idx;
static rx_state_t s_rx_state;
static uint32_t   s_rx_resyncs;

/* CHK 필드(끝에서 6번째부터 4자) 파싱. 비-hex 이면 false. */
static bool parse_hex4(const uint8_t *p, uint16_t *out)
{
  uint16_t v = 0u;
  for (int i = 0; i < 4; i++) {
    uint8_t c = p[i];
    uint16_t d;
    if      (c >= '0' && c <= '9') { d = (uint16_t)(c - '0'); }
    else if (c >= 'A' && c <= 'F') { d = (uint16_t)(c - 'A' + 10); }
    else if (c >= 'a' && c <= 'f') { d = (uint16_t)(c - 'a' + 10); }
    else { return false; }
    v = (uint16_t)((v << 4) | d);
  }
  *out = v;
  return true;
}

/* 헤더 패턴 검사 — p[0..5] = 'S','T', LEN(십진 3자리), TYPE(R/C/A) (§1).
 *  호출자는 p 에서 최소 6바이트가 확보돼 있음을 보장해야 한다. */
static bool hdr_looks_valid(const uint8_t *p)
{
  if ((p[0] != 'S') || (p[1] != 'T')) { return false; }
  for (int i = 2; i <= 4; i++) {
    if ((p[i] < '0') || (p[i] > '9')) { return false; }
  }
  char t = (char)p[5];
  return (t == UART_FRAME_TYPE_RX)
      || (t == UART_FRAME_TYPE_CMD)
      || (t == UART_FRAME_TYPE_ACK);
}

/* ★재동기: 모아둔 버퍼 안에서 다음 'ST' 후보를 찾아 앞으로 당긴다.
 *  560B 를 통째로 버리면, 잘린 프레임 뒤에 붙어 온 **진짜 프레임의 머리**까지
 *  같이 버려서 한 프레임을 더 잃는다. (완료조건 ③ 이 이걸 본다)
 *
 *  ★후보 판정에 헤더 패턴까지 본다 (2026-07-31 강화):
 *   'ST' 는 payload 안에도 흔하다(예: 명령 "STAT"). 위치만 보고 채택하면
 *   가짜마다 **560B 를 다 모은 뒤에야** ED/CRC 로 거부한다(115200 에서 48.6ms).
 *   ※1차 시도는 `s_rx_idx == 6` 시점 검사였는데, **재동기 직후엔 버퍼에 이미
 *     수백 바이트가 있어 그 지점을 건너뛰어 무효**였다(시뮬레이터가 잡음).
 *     → 검사를 **후보 판정 시점**으로 옮긴 것이 이 코드다. */
static void rx_resync(void)
{
  s_rx_resyncs++;

  uint16_t i = 1u;                        /* [0] 은 이미 소비된 'S' */
  while (i < s_rx_idx) {
    if (s_rx[i] == 'S') {
      uint16_t avail = (uint16_t)(s_rx_idx - i);
      if (avail >= 6u) {
        if (hdr_looks_valid(&s_rx[i])) { break; }   /* 진짜 같은 머리 */
        /* 가짜 → 계속 탐색 */
      } else if ((avail < 2u) || (s_rx[i + 1u] == 'T')) {
        break;      /* 아직 판단 불가 → 일단 채택, 이후 검사에 맡긴다 */
      }
    }
    i++;
  }

  if (i >= s_rx_idx) {                    /* 후보 없음 → 처음부터 */
    s_rx_idx   = 0u;
    s_rx_state = ST_WAIT_S;
    return;
  }

  uint16_t n = (uint16_t)(s_rx_idx - i);
  memmove(s_rx, &s_rx[i], n);
  s_rx_idx   = n;
  s_rx_state = (n >= 2u) ? ST_COLLECT : ST_WAIT_T;
}

uart_frame_rx_result_t uart_frame_rx_feed(uint8_t b, uart_frame_rx_t2 *out)
{
  switch (s_rx_state) {
    case ST_WAIT_S:
      if (b == 'S') { s_rx[0] = b; s_rx_idx = 1u; s_rx_state = ST_WAIT_T; }
      return UART_FRAME_RX_NONE;

    case ST_WAIT_T:
      if (b == 'T')      { s_rx[1] = b; s_rx_idx = 2u; s_rx_state = ST_COLLECT; }
      else if (b == 'S') { s_rx[0] = b; s_rx_idx = 1u; }   /* 'SS' → 뒤쪽 S 가 머리 */
      else               { s_rx_idx = 0u; s_rx_state = ST_WAIT_S; }
      return UART_FRAME_RX_NONE;

    case ST_COLLECT:
    default:
      s_rx[s_rx_idx++] = b;

      /* 스트리밍 중(재동기 아님) 6B 가 모인 시점의 헤더 검사.
       * 재동기 경로는 rx_resync() 의 후보 판정이 이미 같은 검사를 한다. */
      if (s_rx_idx == 6u) {
        if (!hdr_looks_valid(s_rx)) {
          if (out != NULL) {
            out->type = (char)s_rx[5]; out->len = 0u;
            out->data = NULL; out->calc_crc = 0u; out->recv_crc = 0u;
          }
          rx_resync();
          return UART_FRAME_RX_BAD_HDR;
        }
      }

      if (s_rx_idx < UART_FRAME_TOTAL_LEN) {
        return UART_FRAME_RX_NONE;
      }
      break;
  }

  /* --- 560B 완성 → 검증 -------------------------------------------------- */
  if (out != NULL) {
    out->type = (char)s_rx[5];
    out->data = (const char *)&s_rx[6];
    out->len  = 0u;
    out->calc_crc = 0u;
    out->recv_crc = 0u;
  }

  /* ED (끝 2바이트) */
  if ((s_rx[UART_FRAME_TOTAL_LEN - 2u] != 'E')
      || (s_rx[UART_FRAME_TOTAL_LEN - 1u] != 'D')) {
    rx_resync();
    return UART_FRAME_RX_BAD_ED;
  }

  /* CHK — 대상 = LEN+TYPE+DATA+DUMMY = offset 2 부터 552B (§1.2) */
  uint16_t calc = uart_frame_crc16(&s_rx[2], UART_FRAME_CRC_SCOPE_LEN);
  uint16_t recv = 0u;
  bool     hex_ok = parse_hex4(&s_rx[UART_FRAME_TOTAL_LEN - 6u], &recv);

  if (out != NULL) { out->calc_crc = calc; out->recv_crc = recv; }

  if (!hex_ok || (calc != recv)) {
    rx_resync();
    return UART_FRAME_RX_BAD_CRC;
  }

  /* LEN (십진 3자리) */
  uint16_t len = 0u;
  for (int i = 2; i <= 4; i++) {
    if ((s_rx[i] < '0') || (s_rx[i] > '9')) { rx_resync(); return UART_FRAME_RX_BAD_LEN; }
    len = (uint16_t)((len * 10u) + (uint16_t)(s_rx[i] - '0'));
  }
  if (len > UART_FRAME_DATA_LEN) { rx_resync(); return UART_FRAME_RX_BAD_LEN; }

  if (out != NULL) { out->len = len; }

  s_rx_idx   = 0u;
  s_rx_state = ST_WAIT_S;
  return UART_FRAME_RX_OK;
}

uint32_t uart_frame_rx_resyncs(void)
{
  return s_rx_resyncs;
}

#if FS_S4 >= 2
size_t uart_frame_build_ack(char *out, size_t out_len,
                            const char *cmd, bool ok, const char *detail)
{
  if ((out == NULL) || (cmd == NULL) || (out_len < UART_FRAME_TOTAL_LEN)) {
    return 0u;
  }
  if (detail == NULL) { detail = ""; }

  size_t dlen = strlen(detail);
  if (dlen > UART_FRAME_ACK_DETAIL_MAX) { dlen = UART_FRAME_ACK_DETAIL_MAX; }

  char *p = out;

  *p++ = 'S'; *p++ = 'T';
  char *len_field = p; p += 3;
  *p++ = UART_FRAME_TYPE_ACK;

  char *data = p;
  p = put_padded(p, cmd, UART_FRAME_ACK_CMD_LEN);
  *p++ = ',';
  p = put_padded(p, ok ? "OK" : "ERR", UART_FRAME_ACK_RESULT_LEN);
  *p++ = ',';

  for (size_t i = 0; i < dlen; i++) { *p++ = detail[i]; }

  /* LEN = 유효분 (detail 패딩 제외) */
  (void)put_u_dec(len_field, (uint32_t)(UART_FRAME_ACK_PREFIX_LEN + dlen), 3);

  while ((size_t)(p - data) < UART_FRAME_DATA_LEN) { *p++ = ' '; }
  for (size_t i = 0; i < UART_FRAME_DUMMY_LEN; i++) { *p++ = ' '; }

  uint16_t crc = uart_frame_crc16((const uint8_t *)len_field,
                                  UART_FRAME_CRC_SCOPE_LEN);
  p = put_hex(p, crc, 4);

  *p++ = 'E'; *p++ = 'D';
  return (size_t)(p - out);
}
#endif /* FS_S4 >= 2 */

#endif /* FS_N >= 4 */

#endif /* FS_N >= 2 */
