/***************************************************************************//**
 * @file    board_cfg.h
 * @brief   NetAnalyzer_FG23_V01 보드 상수 (A3125LT 모듈 + Network Analyzer 연결보드 V01)
 *
 *  ★본 장비 = 비배터리 상시전원(연결보드 5V → 온보드 LDO 3.3V).
 *    감지기(LTD_W10D)와 달리 저전력 로직(수동 EM2 · WOR · TCXO 게이팅)을 쓰지 않는다.
 *    → HFXO 상시 ON / Power Manager 미설치(EM0 상시 active) / 채널당 상시 RX.
 *    근거: `작업지시_ClaudeCode_FG23_프로젝트생성.md` §2.
 *
 *  ★핀 근거:
 *   - TCXO_VCC (PC09) : LTD_W10D_V03 `pin_map.h` §TCXO_VCC
 *       "EFR32 U1.PC09 -> TCXO-VCC 라벨 -> X1(39MHz DSB211SDN)의 VCC 핀.
 *        PC09 는 모듈 외부 핀이 아니지만 MCU 펌웨어가 토글하면 모듈 내부 TCXO ON/OFF."
 *   - UART (PB01/PB02) : `CONNECTION_BOARD_DESIGN.md` §3
 *       "FG23 GPIO_B01(TX) --[33Ω]--> pin11(UART0_RX) → M1S /dev/ttyS1 RX
 *        FG23 GPIO_B02(RX) <--[33Ω]-- pin13(UART0_TX) ← M1S /dev/ttyS1 TX"
 *       ※ 감지기 보드에서는 이 두 핀이 IR_CHECK / ADDRESS_ID5 로 재할당돼 있으나
 *         (`pin_map.h` 1V4_260626), 본 장비는 그 기능이 없어 UART 로 되돌아온다.
 ******************************************************************************/
#ifndef BOARD_CFG_H
#define BOARD_CFG_H

#include "em_gpio.h"   /* gpioPortC 등 — 이 헤더만 include 해도 쓰이도록 자립시킨다 */

/* --- TCXO 전원 게이팅 핀 (모듈 내부 39MHz TCXO VCC) ------------------------
 *  ★본 장비는 상시전원 → 부팅 시 ON 후 **끄지 않는다**(게이팅 없음).
 *   감지기는 EM2 진입 시 OFF 해서 1.2µA 를 달성했으나, 여기서는 RF 안정·최대
 *   성능이 우선이라 상시 ON 이 맞다. (TCXO 재기동 settle 지연 = 수신 놓침 위험) */
#define BOARD_TCXO_VCC_PORT     gpioPortC
#define BOARD_TCXO_VCC_PIN      9u        /* PC09 */

/* TCXO 기동 settle 대기. main 이전(생성자)에 도는 코드라 sleeptimer 사용 불가 →
 * busy loop. 값은 V03 검증치를 그대로 계승(39MHz TCXO 기동 ~수 ms). */
#define BOARD_TCXO_SETTLE_LOOPS 40000u

/* --- UART (FG23 → M1S /dev/ttyS1, 115200 8-N-1) ---------------------------
 *  ★실제 설정 주체는 Studio 컴포넌트(`config/sl_iostream_eusart_vcom_config.h`).
 *   아래는 **교차검증용 기대값**이며, 불일치 시 컴파일 타임에 잡는다.
 *   (static assert 는 `app_init.c` 상단 — Studio config 헤더를 include 하는 곳)
 *   Studio 재생성이 핀을 되돌리는 사고가 실제로 있었으므로 가드를 둔다.
 *
 *  ※ 포트는 여기에 두지 않는다. Studio 는 sl_gpio 도메인(SL_GPIO_PORT_B)을 쓰고
 *    아래 TCXO 상수는 emlib 도메인(gpioPortC)이라, 두 열거값을 섞어 비교하면
 *    "같은 값이겠지" 라는 추측이 들어간다. 포트 검증은 app_init.c 에서
 *    Studio 설정끼리(sl_gpio) 비교한다. */
#define BOARD_UART_TX_PIN       1u        /* PB01 → 연결보드 pin11 (M1S RX) */
#define BOARD_UART_RX_PIN       2u        /* PB02 ← 연결보드 pin13 (M1S TX) */

/* ★보율 115200 → 921600 (2026-07-31, 소장 결정)
 *  - FG23 자체는 1.5M 까지 가능하나 **USB-UART 부품이 1M 상한** → 표준 보율 921600.
 *  - 560B 프레임 전송: 48.6ms → 6.1ms (8배)
 *
 *  ⚠ 39MHz 에서 921600 은 정확히 안 떨어진다. 실제 달성 보율:
 *      OVS16 (현재) : 928,571  → 오차 **+0.756%**
 *      OVS8         : 923,077  → 오차 +0.160%
 *    iostream 은 `EUSART_UART_INIT_DEFAULT_HF`(= eusartOVS16, em_eusart.h:692)를
 *    쓰고 설정 헤더에 OVS 항목이 없다 → 현재 OVS16.
 *    0.756% 는 8-N-1 허용치(이론 5.26%, 실무 2~3%) 안이라 문제없을 것으로 보나,
 *    **프레임 깨짐이 생기면 여기가 첫 번째 용의자**다. 그때 OVS8 강제를 검토한다
 *    (CFG0 를 다시 만지려면 비활성화→OVS 변경→보율 재설정→재활성화 필요).
 *    참고: 115200 일 때 오차는 +0.012% 였다.
 *
 *  [원본 -- #define BOARD_UART_BAUDRATE     115200u] */
#define BOARD_UART_BAUDRATE     921600u

/* --- HFXO (외부 39MHz TCXO) ------------------------------------------------
 *  실제 값은 `config/sl_clock_manager_oscillator_config.h`(Studio). 기대값만 기록.
 *   MODE=XTAL / FREQ=39000000 / CTUNE=140 / CTUNE_MFG_EN=1 */
#define BOARD_HFXO_FREQ_HZ      39000000u
#define BOARD_HFXO_CTUNE        140u

#endif /* BOARD_CFG_H */
