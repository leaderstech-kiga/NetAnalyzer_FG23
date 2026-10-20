/***************************************************************************//**
 * @file    board_cfg.h
 * @brief   NetAnalyzer_FG23_V01 보드 상수 (A3125LT 모듈 + Network Analyzer 연결보드 V01)
 *
 *  ★보드 개정 — V01 → Network_Analyzer_2V0 (도면 2026-09-30 / 대조 2026-10-14)
 *    A3125LT 의 **C 포트 블록이 통째로 바뀌었다.** 40핀 헤더·UART 결선·전원은 불변.
 *    V01 은 채널/주소/모드 DIP 를 FG23 이 직접 읽는 구조라 핀 10개를 점유했는데,
 *    `FG23_UART_인터페이스.md §4.5`(2026-09-21) 결정으로 DIP 를 전부 제거하고
 *    그 자리에 전원제어 1 + 전류감시 2 + 어댑터감지 1 = 4개를 배치했다.
 *      §4.5: "A3125LT 핀 부족으로 스니퍼 연결보드의 채널/모드 DIP 을 제거한다.
 *             하드웨어 부팅 초기값을 두지 않고 소프트웨어로만 설정한다."
 *
 *      [원본 -- V01] PC01=CHNNEL_ID4 · PC02=CHNNEL_ID3 · PC03=CHNNEL_ID2
 *                    PC04=CHNNEL_ID1 · PC05=ADDRESS_ID4 · PC06~08=ADDRESS_ID3~1
 *                    PA09=MODE1 · PA10=MODE2
 *      [2V0]         PC01=POW_EN_CON · PC02=CUR_SCL · PC03=CUR_SDA
 *                    PC04=ADAPTER_IN · PC05=DATA7 · PC06~08=미접속
 *                    PA09·PA10=미접속
 *
 *    ★본 펌웨어 영향 = 0 (2026-10-14 확인). DIP 읽기 코드가 없고(`dip_read.c` 부재),
 *      TCXO(PC09)·UART(PB01/PB02)는 V01/2V0 **불변**이다.
 *      §4.5 원칙("FG23 무상태 · 앱이 유일한 진실의 원천")을 처음부터 지킨 덕분이다.
 *      UART 가 움직였다면 `app_init.c` 의 `_Static_assert` 3개가 빌드를 깼을 것이다.
 *    근거: `../../../Network_Analyzer/CONNECTION_BOARD_DESIGN.md` §0.1
 *          (양 도면 300dpi 렌더 후 모듈 심볼 확대 판독)
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

/* --- UART (FG23 → M1S /dev/ttyS1, 921600 8-N-1) ---------------------------
 *  [원본 -- "115200 8-N-1"] 2026-07-31 상향 때 아래 BOARD_UART_BAUDRATE 만 고치고
 *   이 블록 제목이 115200 으로 남아 있었다(2026-10-15 정정).
 *   메모 §5 "로그가 설정과 어긋나면 안 된다" 가 헤더 주석에서 재현된 경우다.
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

/* --- 2V0 신규 핀 (★상수 정의만 — 구동 코드는 각 담당 스텝에서) --------------
 *  ★여기 있는 핀은 **아직 어떤 코드도 쓰지 않는다.** S11/S12/S14 에서 도면을
 *   다시 찾지 않도록 미리 기록해 두는 것이다. GPIO 모드 설정·구동은 각 스텝 몫.
 *
 *  ⚠ **Studio config 에 대응 항목이 없다**(UART 만 있다) → UART 처럼
 *    `_Static_assert` 로 대조할 상대가 없다. 핀이 틀리면 컴파일이 아니라
 *    **실기에서** 드러난다. 각 스텝 착수 시 §0.1 표와 한 번 더 대조할 것.
 *
 *    EFR32   신호          방향                                  담당 스텝
 *    ------------------------------------------------------------------------
 *    PC00    KEY           입력  active-LOW (풀업 필요)           S14b
 *    PC01    POW_EN_CON    출력  push-pull, **HIGH = 전원 유지**  S14a ← 이번
 *    PC02    CUR_SCL       I2C   SCL (INA219 slave 0x40)          S11a ← 구현됨
 *    PC03    CUR_SDA       I2C   SDA                              S11a ← 구현됨
 *    PC04    ADAPTER_IN    입력  **HIGH = 플러그 삽입**           S12
 *
 *  근거: `CONNECTION_BOARD_DESIGN.md` §0.1 "2V0 에서 새로 쓸 핀 (구현 예정)"
 *        — `POW_EN_CON`(모듈 핀2)은 포트 심볼 화살표가 모듈 바깥 방향 = 출력 확인
 *  상세: `FG23_UART_프로토콜_v2_확장안_20261014.md`
 *        §0.2(전원) · §3.4(INA219) · §2.3(어댑터) */

/* KEY (PC00) — S1 스위치. active-LOW (S1 은 GND 로만 당기고 외부 풀업이 없다).
 * ⚠ 내부 풀업이 필요하지만 **S14b 범위다. 이번(S14a)에 켜지 마라.** */
#define BOARD_KEY_PORT          gpioPortC
#define BOARD_KEY_PIN           0u        /* PC00 */

/* POW_EN_CON (PC01) — ★S14a 전원 자기유지 래치. push-pull **HIGH = 전원 유지**.
 *
 *  ★`POW_EN` 과 `POW_EN_CON` 은 **다른 네트다.** 혼동하면 측정도 판단도 틀린다:
 *    POW_EN      = 보드 래치 노드, **11.45 V**, U4(LMR33630) EN 으로 직행
 *    POW_EN_CON  = FG23 GPIO,      **3.3 V**,  D11 을 통해 POW_EN 을 끌어올림
 *
 *  전기값 (작업지시 §1 표 / 2V0 Sheet 1·4):
 *    S2 누름 중  POW_EN = 12.6 x 100k/(10k+100k)      = **11.45 V**
 *    홀드업      tau = R39*C16 = 100k x 10uF = 1.0 s
 *                t = tau*ln(11.45 / 1.231) = **2.2 초**  <- U4 EN 임계 1.231 V
 *    C16 충전    tau = R38 10k x 10uF = 0.1 s
 *                -> **S2 를 0.5초 이상** 눌러야 홀드업이 full 로 찬다
 *    래치 성립 시 POW_EN = (3.3 - 0.3_Schottky) x 100k/101k = **2.97 V**
 *                -> 임계 1.231 V 대비 충분
 *
 *  ★D11(PMEG3010ER)의 역할 = **GPIO 보호**다. 자폭 방지가 아니다.
 *    POW_EN 은 11.45 V 인데 FG23 GPIO 는 3.3 V 소자다. D11 이 역류를 막아주므로
 *    **push-pull 로 밀어도 안전**하다(오픈드레인 불필요).
 *
 *  [!] **자폭은 가능하다.** POW_EN_CON 을 LOW 로 구동하면 D11 이 역바이어스되고
 *     POW_EN 이 R39 로 방전되어 **2.2초 뒤 전원이 꺼진다.**
 *     -> **이번 스텝(S14a)에서 POW_EN_CON 을 LOW 로 만드는 코드를 넣지 마라.**
 *       끄는 기능은 S14c(PWROFF) 다. (작업지시 §7)
 *     -> S5-5 DIP 는 **개발 중 임시**이고 운용 시 OFF 다. 거기 기대면 안 된다.
 *     ※ 홀드업 2.2초 동안 MCU 는 살아 있으므로 **순간 글리치는 살아남고,
 *        지속적 LOW 만 차단된다.**
 *
 *  ⚠ 워치독이 없다(2026-10-14 확인, 2026-10-15 재확인). MCU 가 행(hang)하면
 *    POW_EN_CON 이 HIGH 로 남아 **영원히 켜진 채 배터리가 방전된다.**
 *    최종 수단 = 배터리 커넥터 **J6 분리**.
 *
 *  [원본 -- 2026-10-14 작성분. 2026-10-15 소장 정정 반영]
 *    #define BOARD_POW_EN_PORT / BOARD_POW_EN_PIN
 *    "⚠ S14a 구현 시: 출력값을 HIGH 로 먼저 쓰고 나서 모드를 push-pull 로 올릴 것.
 *      순서가 뒤바뀌면 설정 순간 LOW 가 나가 자기 전원을 끊는다."
 *    -> (1) 이름이 보드 네트(POW_EN)와 겹쳐 혼동을 유발했다
 *       (2) GPIO_PinModeSet(port,pin,mode,out) 이 out 을 함께 받으므로 순서 문제가
 *           애초에 없다 - 불필요하게 복잡한 경고였다
 *       (3) V_SYS 를 12.6V 가 아닌 3.7V/5V 로 **추측**해 홀드업을 2.2초가 아닌
 *           1초로 잘못 적었고, D11 을 "자폭 방지"로 오독했다 (실제는 GPIO 보호) */
#define BOARD_POW_EN_CON_PORT   gpioPortC
#define BOARD_POW_EN_CON_PIN    1u        /* PC01 */

/* 전류 감시 I2C (INA219, slave 0x40) — S11a 구현 완료 (2026-10-16, `ina219.c`)
 *  ★외부 풀업 R57/R58 = 10kΩ 이 있다 → **내부 풀업 금지**, open-drain(gpioModeWiredAnd).
 *  ★라우팅은 핀툴이 아니라 코드에서 한다 (GPIO->I2CROUTE[0]).
 *    핀툴을 쓰면 `config/pin_config.h` 가 재작성되는데 거기에 UART PB01/PB02 가 있다.
 *  ★I2C0 기능클럭 = LSPCLK = HCLK/PCLKDIV/2 ≈ 19.5MHz. 선택 레지스터가 없어 고정이다
 *    (I2C1 은 이 분기가 아니다 — 바꾸려면 em_cmu.c 의 클럭 계산부터 다시 볼 것). */
#define BOARD_CUR_SCL_PORT      gpioPortC
#define BOARD_CUR_SCL_PIN       2u        /* PC02 */
#define BOARD_CUR_SDA_PORT      gpioPortC
#define BOARD_CUR_SDA_PIN       3u        /* PC03 */

/* ADAPTER_IN (PC04) — 어댑터 플러그 감지. HIGH = 삽입. **S12 범위.**
 *  잭 J7 내장 스위치: 플러그 없으면 pin2-pin3 단락 -> LOW / 삽입 -> 개방 -> R34 가 풀업.
 * ⚠ **내부 풀업 켜지 마라** - 외부 R34 10K 와 병렬이 되어 잭 접점 전류가 늘어난다.
 * ⚠ **내부 풀다운 절대 금지** - R34 와 분압되어 판정이 깨진다. */
#define BOARD_ADAPTER_IN_PORT   gpioPortC
#define BOARD_ADAPTER_IN_PIN    4u        /* PC04 */

/* --- HFXO (외부 39MHz TCXO) ------------------------------------------------
 *  실제 값은 `config/sl_clock_manager_oscillator_config.h`(Studio). 기대값만 기록.
 *   MODE=XTAL / FREQ=39000000 / CTUNE=140 / CTUNE_MFG_EN=1 */
#define BOARD_HFXO_FREQ_HZ      39000000u
#define BOARD_HFXO_CTUNE        140u

#endif /* BOARD_CFG_H */
