/***************************************************************************//**
 * @file    cmd_parse.c
 * @brief   M1S → FG23 명령 해석 + ACK 회신
 ******************************************************************************/

#include "cmd_parse.h"

#if (FS_N >= 4) && (FS_S4 >= 2)

#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "uart_frame.h"

/** 펌웨어 버전 문자열 (VER 응답) — ASCII 전용 */
#define FW_VERSION_STR   "NetAnalyzer_FG23_V01 FS4c"

/* 기본값 = 안전측: passive, 데이터 채널 0, short PHY. */
static cmd_state_t s_state = {
  .ch = 0u, .phy_long = false, .active = false, .scan = {0}, .scan_n = 0u,
};

/* ACK 조립 버퍼. 560B 를 스택에 두면 SL_STACK_SIZE(2048) 를 크게 잠식한다.
 * 슈퍼루프 단일 흐름이라 동시에 2개를 만들 일이 없어 정적 1개로 충분. */
static char s_ack[UART_FRAME_TOTAL_LEN];

const cmd_state_t *cmd_state(void)
{
  return &s_state;
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void send_ack(const char *cmd, bool ok, const char *detail)
{
  size_t n = uart_frame_build_ack(s_ack, sizeof(s_ack), cmd, ok, detail);
  if (n == UART_FRAME_TOTAL_LEN) {
    app_log_raw(s_ack, n);
  }
}

#if FS_S4 >= 3
/* 채널 유효성 — radioconf 에 실제로 존재하는 채널만 (§G / rail_config.c).
 *  데이터 0~24 (짧은PA) / wake 100~124 (롱PA). 25~99 는 존재하지 않는다.
 *  "0~124" 로 뭉뚱그리면 없는 채널을 받아들여 S6 에서 조용히 실패한다. */
#define RF_CH_DATA_MAX    24u
#define RF_CH_WAKE_BASE  100u
#define RF_CH_WAKE_MAX   124u

static bool ch_is_valid(uint16_t ch)
{
  return (ch <= RF_CH_DATA_MAX)
      || ((ch >= RF_CH_WAKE_BASE) && (ch <= RF_CH_WAKE_MAX));
}

/* 십진 문자열 → uint16. 전부 숫자여야 하고 비어 있으면 실패. */
static bool parse_u16(const char *s, uint16_t *out)
{
  if ((s == NULL) || (s[0] == '\0')) { return false; }
  uint32_t v = 0u;
  for (const char *p = s; *p != '\0'; p++) {
    if ((*p < '0') || (*p > '9')) { return false; }
    v = (v * 10u) + (uint32_t)(*p - '0');
    if (v > 65535u) { return false; }
  }
  *out = (uint16_t)v;
  return true;
}
#endif /* FS_S4 >= 3 */

/* DATA 는 NUL 종단이 아니고 뒤가 공백 패딩이다 → 앞쪽 토큰만 안전하게 뽑는다.
 *
 *  @param[out] consumed  **선행 공백까지 포함해** src 에서 소비한 바이트 수 (NULL 허용)
 *  @return 토큰 길이. tok 는 항상 NUL 종단.
 *
 *  ★consumed 를 따로 내는 이유: 토큰 길이만 받아 `src + tok_len` 으로 다음 위치를
 *   잡으면 **선행 공백만큼 어긋난다**(예 " CH 3" → 'H 3' 을 인자로 착각).
 *   호출부가 공백을 직접 세게 하면 같은 실수가 여러 군데로 번진다. */
static size_t take_token(const char *src, size_t len, char *tok, size_t tok_sz,
                         size_t *consumed)
{
  size_t i = 0, o = 0;

  while ((i < len) && (src[i] == ' ')) { i++; }          /* 선행 공백 */
  while ((i < len) && (src[i] != ' ') && (src[i] != '\0')
         && (o + 1u < tok_sz)) {
    tok[o++] = src[i++];
  }
  tok[o] = '\0';

  if (consumed != NULL) { *consumed = i; }
  return o;
}

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void cmd_parse_handle(const char *data, uint16_t len)
{
  if ((data == NULL) || (len == 0u)) {
    send_ack("?", false, "empty");
    return;
  }
  if (len > UART_FRAME_DATA_LEN) { len = UART_FRAME_DATA_LEN; }

  char   cmd[17];
  size_t cmd_end = 0u;                            /* 선행공백 포함 소비량 */
  size_t clen = take_token(data, len, cmd, sizeof(cmd), &cmd_end);

  if (clen == 0u) {
    send_ack("?", false, "empty");
    return;
  }

  /* ---- S4b: 조회 명령 (상태 안 바꿈) ------------------------------------- */
  if (strcmp(cmd, "VER") == 0) {
    send_ack(cmd, true, FW_VERSION_STR);
    return;
  }

  if (strcmp(cmd, "STAT") == 0) {
    char detail[96];
    int n = snprintf(detail, sizeof(detail),
                     "ch=%03u phy=%s mode=%s scan_n=%u",
                     (unsigned)s_state.ch,
                     s_state.phy_long ? "long" : "short",
                     s_state.active   ? "active" : "passive",
                     (unsigned)s_state.scan_n);
    if (n < 0) { detail[0] = '\0'; }
    send_ack(cmd, true, detail);
    return;
  }

  /* ---- ★안전: TX 는 S9 까지 명시 거절 (무시하지 않는다) ------------------ */
  if (strcmp(cmd, "TX") == 0) {
    send_ack(cmd, false, "not_supported (TX enabled from S9)");
    return;
  }

  /* ---- S4c: 상태 변경 명령 ------------------------------------------------ */
#if FS_S4 >= 3
  const char *arg  = data + cmd_end;              /* ★선행공백까지 건너뛴 뒤 */
  size_t      alen = (len > cmd_end) ? (size_t)(len - cmd_end) : 0u;

  if (strcmp(cmd, "CH") == 0) {
    char t[8];
    if (take_token(arg, alen, t, sizeof(t), NULL) == 0u) {
      send_ack(cmd, false, "missing arg"); return;
    }
    uint16_t v;
    if (!parse_u16(t, &v) || !ch_is_valid(v)) {
      send_ack(cmd, false, "invalid (valid: 0-24 data, 100-124 wake)"); return;
    }
    s_state.ch       = v;
    s_state.phy_long = (v >= RF_CH_WAKE_BASE);    /* §1.1: ch 가 PHY 를 인코딩 */

    char d[48];
    (void)snprintf(d, sizeof(d), "ch=%03u phy=%s", (unsigned)v,
                   s_state.phy_long ? "long" : "short");
    send_ack(cmd, true, d);
    return;
  }

  if (strcmp(cmd, "PHY") == 0) {
    char t[8];
    if (take_token(arg, alen, t, sizeof(t), NULL) == 0u) {
      send_ack(cmd, false, "missing arg (short|long)"); return;
    }
    bool want_long;
    if      (strcmp(t, "short") == 0) { want_long = false; }
    else if (strcmp(t, "long")  == 0) { want_long = true;  }
    else { send_ack(cmd, false, "invalid (short|long)"); return; }

    /* ★§1.1 은 "ch 가 PHY 를 인코딩" 이라고 규정한다. 현재 ch 와 모순되는
     *  PHY 를 조용히 받아들이면 불변식이 깨진다 → 명시적으로 거절한다.
     *  (§3.1 의 PHY 명령 자체는 살려 두되, ch 와 어긋나는 조합만 막는 것) */
    bool implied = (s_state.ch >= RF_CH_WAKE_BASE);
    if (want_long != implied) {
      char d[80];
      (void)snprintf(d, sizeof(d),
                     "conflicts with ch=%03u (implies %s) - change CH first",
                     (unsigned)s_state.ch, implied ? "long" : "short");
      send_ack(cmd, false, d);
      return;
    }
    s_state.phy_long = want_long;
    send_ack(cmd, true, want_long ? "phy=long" : "phy=short");
    return;
  }

  if (strcmp(cmd, "MODE") == 0) {
    char t[10];
    if (take_token(arg, alen, t, sizeof(t), NULL) == 0u) {
      send_ack(cmd, false, "missing arg (passive|active)"); return;
    }
    if      (strcmp(t, "passive") == 0) { s_state.active = false; }
    else if (strcmp(t, "active")  == 0) { s_state.active = true;  }
    else { send_ack(cmd, false, "invalid (passive|active)"); return; }

    send_ack(cmd, true, s_state.active ? "mode=active" : "mode=passive");
    return;
  }

  if (strcmp(cmd, "SCAN") == 0) {
    /* SCAN <ch1..chN> — 1~4개 (§4.1). 5개 이상은 거절. */
    uint8_t  list[4];
    uint8_t  n    = 0u;
    size_t   off  = 0u;
    char     t[8];

    while (off < alen) {
      size_t used = 0u;
      size_t tl = take_token(arg + off, alen - off, t, sizeof(t), &used);
      if (tl == 0u) { break; }                    /* 남은 게 공백뿐 */
      off += used;                                /* ★공백 포함 소비량 */

      if (n >= 4u) { send_ack(cmd, false, "too many (max 4)"); return; }

      uint16_t v;
      if (!parse_u16(t, &v) || !ch_is_valid(v)) {
        send_ack(cmd, false, "invalid ch (0-24, 100-124)"); return;
      }
      list[n++] = (uint8_t)v;
    }

    if (n == 0u) { send_ack(cmd, false, "missing arg (1-4 channels)"); return; }

    for (uint8_t i = 0; i < n; i++) { s_state.scan[i] = list[i]; }
    s_state.scan_n = n;

    char d[48];
    (void)snprintf(d, sizeof(d), "scan_n=%u (rotation from S8)", (unsigned)n);
    send_ack(cmd, true, d);
    return;
  }
#else
  if ((strcmp(cmd, "CH")   == 0) || (strcmp(cmd, "PHY")  == 0)
      || (strcmp(cmd, "MODE") == 0) || (strcmp(cmd, "SCAN") == 0)) {
    send_ack(cmd, false, "not_yet (S4c)");
    return;
  }
#endif

  send_ack(cmd, false, "unknown_cmd");
}

#endif /* FS_N >= 4 && FS_S4 >= 2 */
