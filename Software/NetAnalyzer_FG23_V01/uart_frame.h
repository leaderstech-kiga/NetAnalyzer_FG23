/***************************************************************************//**
 * @file    uart_frame.h
 * @brief   FG23 ↔ M1S UART 프레임 (560B 고정) — 스펙 단일 기준 구현
 *
 *  ★기준 문서: `Network_Analyzer/FG23_UART_인터페이스.md`
 *   §1   프레임 envelope (ST|LEN(3)|TYPE|DATA(540)|DUMMY(8)|CHK(4)|ED = 560B)
 *   §1.1 DATA = RX CSV 13필드 (고정 자리수)
 *   §1.2 CHK = CRC-16/CCITT-FALSE
 *
 *  ★스텝 (step_config.h):
 *   S2  CRC-16/CCITT-FALSE + 자기검증          ← 현재
 *   S3  560B 프레임 조립 (더미 CSV) + 논블로킹 TX
 *   S4  명령 파싱(TYPE='C') + ACK(TYPE='A')
 *   S7  v1.1 파서 → CSV 13필드
 ******************************************************************************/
#ifndef UART_FRAME_H
#define UART_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "step_config.h"

#if FS_N >= 2

/* --- 프레임 상수 (FG23_UART_인터페이스.md §1) ------------------------------
 *  S3 에서 쓰지만, 스펙 값이므로 여기 한 곳에만 둔다(중복 정의 방지). */
#define UART_FRAME_DATA_LEN   540u   /* DATA 고정 길이 */
#define UART_FRAME_DUMMY_LEN    8u   /* 예비 */
#define UART_FRAME_TOTAL_LEN  560u   /* ST(2)+LEN(3)+TYPE(1)+DATA+DUMMY+CHK(4)+ED(2) */

/* CRC 대상 범위 = LEN + TYPE + DATA + DUMMY (START·END 제외) = 552B (§1.2) */
#define UART_FRAME_CRC_SCOPE_LEN  (3u + 1u + UART_FRAME_DATA_LEN + UART_FRAME_DUMMY_LEN)

/* --- 표준 검증 벡터 (§1.2) ------------------------------------------------- */
#define UART_FRAME_CRC_VECTOR_123456789   0x29B1u   /* 자기검증 (필수) */
#define UART_FRAME_CRC_VECTOR_ABC         0xF508u   /* 짧은 확인용 */

/***************************************************************************//**
 * CRC-16/CCITT-FALSE 계산.
 *
 *  poly=0x1021 / init=0xFFFF / refin=false / refout=false / xorout=0x0000
 *  근거: `FG23_UART_인터페이스.md` §1.2 (C 샘플 코드 그대로 이식).
 *
 * @param[in] data  대상 바이트열
 * @param[in] len   길이
 * @return CRC-16 값. 프레임 CHK 필드는 이 값을 "%04X" 로 4자 대문자 hex.
 ******************************************************************************/
uint16_t uart_frame_crc16(const uint8_t *data, size_t len);

/***************************************************************************//**
 * CRC 구현 자기검증 (S2 완료조건).
 *
 *  표준 벡터 2개를 확인한다. 하나라도 틀리면 M1S 앱과 프레임 호환이 깨지므로
 *  **여기서 잡아야 한다** — S3 이후 프레임이 안 맞을 때 원인 후보를 미리 제거.
 *
 * @param[out] out_123456789  실제 계산값 (NULL 허용) — FAIL 시 진단용
 * @param[out] out_abc        실제 계산값 (NULL 허용)
 * @return 둘 다 기대값과 일치하면 true
 ******************************************************************************/
bool uart_frame_crc_selftest(uint16_t *out_123456789, uint16_t *out_abc);

#endif /* FS_N >= 2 */

#if FS_N >= 3

/* --- CSV 13필드 고정 자리수 (§1.1) ----------------------------------------
 *  앞 12필드(고정폭) + 콤마12 = 53자, + payload_hex(486) = 539, + 1공백 = DATA 540 */
#define UART_FRAME_PAYLOAD_HEX_LEN  486u                 /* 243B × 2 */
#define UART_FRAME_PAYLOAD_MAX      (UART_FRAME_PAYLOAD_HEX_LEN / 2u)   /* 243B */
#define UART_FRAME_CSV_PREFIX_LEN    53u                 /* 앞 12필드 + 콤마12 */

/* TYPE (§1) */
#define UART_FRAME_TYPE_RX    'R'   /* 수신 패킷 */
#define UART_FRAME_TYPE_CMD   'C'   /* 명령 (M1S → FG23) */
#define UART_FRAME_TYPE_ACK   'A'   /* ACK */

/** RX 프레임 1건 (§1.1 CSV 13필드의 원본 값) */
typedef struct {
  uint32_t       ts_ms;         /* 1. FG23 상대 타임스탬프(ms) */
  uint16_t       ch;            /* 2. 0~24=데이터 / 100~124=wake (§G) */
  int16_t        rssi_dbm;      /* 3. */
  uint8_t        lqi;           /* 4. */
  bool           crc_ok;        /* 5. OK / FAIL */
  uint8_t        ver;           /* 6. 0x02=v1.1 */
  uint8_t        group;         /* 7. GROUP_ID */
  uint8_t        src;           /* 8. */
  uint8_t        msg_type;      /* 9. §7.3 */
  uint16_t       seq;           /* 10. */
  uint8_t        hop;           /* 11. */
  const uint8_t *payload;       /* 13. DATA 원본 (NULL 이면 길이 0 취급) */
  uint16_t       payload_len;   /* 12. len — UART_FRAME_PAYLOAD_MAX 초과분은 절단 */
} uart_frame_rx_t;

/***************************************************************************//**
 * RX 프레임(TYPE='R') 을 560B 고정 프레임으로 조립.
 *
 *  ST|LEN(3)|R|DATA(540)|DUMMY(8)|CHK(4)|ED  — 전부 printable ASCII (§1)
 *
 * @param[out] out      최소 UART_FRAME_TOTAL_LEN 바이트. NUL 종단 안 함.
 * @param[in]  out_len  out 크기 (안전 확인용)
 * @param[in]  rx       원본 값
 * @return 쓴 바이트 수(=560). 실패 시 0.
 ******************************************************************************/
size_t uart_frame_build_rx(char *out, size_t out_len, const uart_frame_rx_t *rx);

/***************************************************************************//**
 * 프레임 조립 자기검증 (S3a 완료조건).
 *
 *  `FG23_UART_인터페이스.md §1.2` 의 예제 프레임을 재현해 CHK == 0xEB7D 인지 본다.
 *  이 벡터가 맞으면 필드 폭·패딩·LEN 계산·CRC 범위가 전부 스펙과 일치한다는 뜻이다.
 *
 * @param[out] out_crc    실제 계산된 CHK (NULL 허용) — FAIL 시 진단용
 * @param[out] out_len    실제 프레임 길이 (NULL 허용)
 * @param[out] out_frame  조립된 프레임 포인터 (NULL 허용) — 육안 확인용, 정적 버퍼
 * @return CHK==0xEB7D 이고 길이==560 이면 true
 ******************************************************************************/
bool uart_frame_build_selftest(uint16_t *out_crc, size_t *out_len,
                               const char **out_frame);

/** 예제 프레임 기대 CHK (§1.2 "검증 예제") */
#define UART_FRAME_EXAMPLE_CRC   0xEB7Du

#endif /* FS_N >= 3 */

#if FS_N >= 4

/** 프레임 수신 결과 */
typedef enum {
  UART_FRAME_RX_NONE = 0,   /**< 아직 프레임이 완성되지 않음 (계속 먹여라) */
  UART_FRAME_RX_OK,         /**< 완전한 프레임 (ED 확인 + CRC 일치) */
  UART_FRAME_RX_BAD_HDR,    /**< ST 직후 6B 사전검사 실패 → 가짜 동기, 즉시 재동기 */
  UART_FRAME_RX_BAD_ED,     /**< 560B 는 모였는데 끝이 'ED' 가 아님 → 재동기 */
  UART_FRAME_RX_BAD_CRC,    /**< CHK 불일치 → 재동기 */
  UART_FRAME_RX_BAD_LEN,    /**< LEN 필드가 범위 밖(>540 또는 비숫자) → 재동기 */
} uart_frame_rx_result_t;

/** 수신된 프레임 (버퍼는 내부 정적 — 다음 feed 전까지만 유효) */
typedef struct {
  char        type;         /**< 'R' / 'C' / 'A' */
  uint16_t    len;          /**< LEN 필드 = DATA 유효 길이 */
  const char *data;         /**< DATA(540) 시작 포인터 */
  uint16_t    calc_crc;     /**< 계산한 CHK */
  uint16_t    recv_crc;     /**< 프레임에 실린 CHK */
} uart_frame_rx_t2;

/***************************************************************************//**
 * 수신 바이트를 1개 먹인다 (프레임 동기 상태기).
 *
 *  ★재동기: 검증 실패 시 버린 구간 **안에서** 다음 'ST' 를 찾아 이어붙인다.
 *   560B 를 통째로 버리면 그 안에 들어있던 진짜 프레임 머리를 놓친다.
 *
 * @param[in]  b    수신 바이트
 * @param[out] out  UART_FRAME_RX_OK 일 때 채워짐 (실패 시에도 crc 필드는 채움)
 * @return 판정 결과
 ******************************************************************************/
uart_frame_rx_result_t uart_frame_rx_feed(uint8_t b, uart_frame_rx_t2 *out);

/** 재동기 발생 누계 (진단용) */
uint32_t uart_frame_rx_resyncs(void);

#if FS_S4 >= 2
/* --- ACK(TYPE='A') DATA 레이아웃 ------------------------------------------
 *  ★스펙 공백을 메운 안 (2026-07-31). `FG23_UART_인터페이스.md §1.1` 은
 *   TYPE='R' 의 DATA 만 규정하고 'C'/'A' 는 비어 있다. §1.1 과 같은 규율
 *   (고정 자리수 + 콤마 구분)로 맞춰, 앱이 RX 파서와 같은 방식으로 읽게 한다.
 *
 *    cmd(16, 좌측정렬) , result(4) , detail(나머지, 좌측정렬)
 *    result = "OK  " / "ERR "  ← 고정폭이라 앱이 위치로 꺼낼 수 있다
 *    LEN    = 22 + strlen(detail)   (패딩 제외 유효분)
 *
 *  예)  VER    -> "VER             ,OK  ,NetAnalyzer_FG23_V01 FS4"
 *       STAT   -> "STAT            ,OK  ,ch=000 phy=short mode=passive"
 *       CH 200 -> "CH              ,ERR ,range 0-124"
 *  ※ 스펙 문서 반영은 Cowork 몫 (§3 에 추가 필요). */
#define UART_FRAME_ACK_CMD_LEN     16u
#define UART_FRAME_ACK_RESULT_LEN   4u
#define UART_FRAME_ACK_PREFIX_LEN  (UART_FRAME_ACK_CMD_LEN + 1u \
                                    + UART_FRAME_ACK_RESULT_LEN + 1u)   /* 22 */
#define UART_FRAME_ACK_DETAIL_MAX  (UART_FRAME_DATA_LEN - UART_FRAME_ACK_PREFIX_LEN)

/***************************************************************************//**
 * ACK(TYPE='A') 560B 프레임 조립.
 *
 * @param[out] out      최소 UART_FRAME_TOTAL_LEN 바이트
 * @param[in]  out_len  out 크기
 * @param[in]  cmd      원 명령 이름 (16자 초과분은 절단)
 * @param[in]  ok       true=OK / false=ERR
 * @param[in]  detail   부가 설명 (NULL 허용)
 * @return 쓴 바이트 수(=560). 실패 시 0.
 ******************************************************************************/
size_t uart_frame_build_ack(char *out, size_t out_len,
                            const char *cmd, bool ok, const char *detail);
#endif /* FS_S4 >= 2 */

#endif /* FS_N >= 4 */
#endif /* UART_FRAME_H */
