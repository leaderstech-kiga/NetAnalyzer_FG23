/***************************************************************************//**
 * @file    cmd_parse.h
 * @brief   M1S → FG23 명령 해석 + ACK 회신 (FG23_UART_인터페이스.md §3)
 *
 *  ★스텝 (step_config.h `FS_S4`):
 *   S4b (FS_S4>=2)  VER / STAT           — 조회만, 상태 안 바꿈
 *   S4c (FS_S4>=3)  CH / PHY / MODE / SCAN — 상태 변경
 *   S9  (FS_N>=9)   TX                    — ★안전: 그 전까지 명시 거절
 *
 *  ★안전 (절대선 — JUDGMENT §1):
 *   기본 MODE = passive. TX 경로는 FS_N<9 에서 컴파일 자체가 안 된다.
 *   그래도 `TX` 명령을 받으면 **무시하지 않고 ERR not_supported 로 회신**한다 —
 *   M1S 앱이 "보냈는데 반응 없음"으로 헤매지 않도록.
 ******************************************************************************/
#ifndef CMD_PARSE_H
#define CMD_PARSE_H

#include <stdint.h>
#include <stdbool.h>

#include "step_config.h"

#if (FS_N >= 4) && (FS_S4 >= 2)

/** FG23 런타임 상태 (STAT 로 조회, S4c 에서 변경 가능) */
typedef struct {
  uint16_t ch;          /**< RX 채널 0~124 (0~24=데이터 / 100~124=wake) */
  bool     phy_long;    /**< false=short(데이터) / true=long(wake) */
  bool     active;      /**< MODE: false=passive(기본·안전) / true=active */
  uint8_t  scan[4];     /**< SCAN 채널 목록 */
  uint8_t  scan_n;      /**< 0=미설정(camp) */
} cmd_state_t;

/** 현재 상태 조회 (읽기 전용) */
const cmd_state_t *cmd_state(void);

/***************************************************************************//**
 * TYPE='C' 프레임의 DATA 를 해석하고 **ACK(TYPE='A') 프레임을 송신**한다.
 *
 * @param[in] data  DATA(540) 시작 포인터 (NUL 종단 아님)
 * @param[in] len   LEN 필드 값 = 유효 길이
 ******************************************************************************/
void cmd_parse_handle(const char *data, uint16_t len);

#endif /* FS_N >= 4 && FS_S4 >= 2 */
#endif /* CMD_PARSE_H */
