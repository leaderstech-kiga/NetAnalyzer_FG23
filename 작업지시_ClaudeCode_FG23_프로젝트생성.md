# 작업지시 (Claude Code) — NetAnalyzer_FG23_V01 프로젝트 생성 ~ 세부 동작

> **프로젝트**: `NetAnalyzer_FG23_V01` (Network Analyzer의 FG23 스니퍼 펌웨어).
> **환경**: Simplicity Studio 5 + Simplicity SDK **2025.6.3**, GNU ARM v12.2.1, MCU **EFR32FG23A010F256GM48**(A3125LT).
> **참조**: 감지기 `../LTD_W10D/Software/LTD_W10D_V03` (drv_rf·rf_proto·radioconf 재활용).
> **★차이 = 비배터리 상시전원(연결보드 5V→LDO 3.3V) → 전류 자유 → 고성능**(저전력 로직 제거).
> **역할 분담**: 프로젝트 생성·컴포넌트·radioconf = **Studio(사용자)** / 코드 = **Claude Code**. 빌드=Studio.
> **원칙**: 한 스텝씩 빌드→검증→다음, `#if` 가드, 원본 주석 보존, RAIL/SDK는 `silicon-labs-docs` MCP 확인.

---

## 0. LTD_W10D_V03 참조 포인트 (그대로 계승)
- **"RAIL - SoC Empty" 예제 기반**(Radio Configurator 포함). ⚠ 빈 프로젝트 금지(교훈: 빈→RAIL 승격 불가).
- 컴포넌트: clock_manager, power_manager, sleeptimer, udelay, emlib(GPIO/EMU/CMU), **SL RAIL Utility(Init/Callbacks/PA)**, radio_config(radioconf), device_init, sl_gpio.
- radioconf: 협력업체 PHY 복사 후 **Studio Save로 재생성**(rail_config 버전 일치, assert 66 회피).
- 부팅: RAIL init에 HFXO 필요 → **SYSCLK=HFXO 부팅**, TCXO는 생성자(`__attribute__((constructor))`)로 main 이전 ON.
- HFXO = 외부 **39MHz TCXO**, 게이팅 핀 TCXO_VCC(PC09).

## 1. 프로젝트 생성 (Studio, 사용자 — Claude Code는 안내)
1. New Project → **"RAIL - SoC Empty"** → EFR32FG23A010F256GM48 → 이름 **`NetAnalyzer_FG23_V01`**.
2. 위치: `Network_Analyzer/Software/NetAnalyzer_FG23_V01/` (감지기와 대칭).
3. 컴포넌트 설치: §0 목록 + **★UART용 추가**(M1S 통신) — `IOStream: USART` 또는 emlib USART(115200 8-N-1).
4. radioconf: LTD_W10D_V03 `radio_settings.radioconf`(**2채널: 데이터 0~24 short + wake 100~124 long**) 복사 → **Save 재생성**.

## 2. ★전원/클럭 설정 — 고성능 (비배터리)
감지기는 저전력(수동 EM2·WOR·TCXO 게이팅, standby 1.2µA)이었으나 **본 장비는 상시전원 → 성능 우선**:
- **SYSCLK = HFXO(39MHz TCXO) 상시 ON** — 저전력 클럭 전환·TCXO 게이팅 **안 함**. RF 안정·최대 성능.
- **Power Manager = EM1까지만**(또는 EM0 active 유지). **수동 EM2 deep sleep·WOR 듀티사이클 미사용.**
- **상시 RX(continuous)** — 감지기 WOR 대신 **채널당 상시 수신** → 패킷 놓침 최소화(스캔은 채널 전환만, §스캔).
- IADC on-demand·누설 최적화 등 저전력 패턴 불필요(있어도 무방, 단순화 위해 제거 권장).
- 부팅 HFXO는 계승하되 **이후 게이팅/EM2 전환 없음**.

## 3. 코드 구조 (Claude Code)
- `drv_rf.c/h`·`rf_proto.c/h`(RX 방향) **재활용**하되 **저전력 로직(EM2/WOR/게이팅) 제거 → 상시 RX**로 개작.
- 신규 모듈: `uart_frame`(560B 프레임 encode/decode + **CRC-16/CCITT-FALSE**), `scan_sched`(1~4채널 순환, wake≤4초), `cmd_parse`(CH/PHY/SCAN/MODE/TX + ACK), `rf_tx`(TOOL 능동 송신).
- `app.c`: 이벤트 디스패치(RF RX → CSV 프레임 UART 출력 / UART 명령 → 처리).

## 4. 세부 동작 작업 (FS1~FS7)
→ **`작업지시_ClaudeCode_FG23스니퍼.md` FS1~FS7** 그대로 수행:
FS1 radioconf+RX / FS2 v1.1 파서→CSV / FS3 UART 프레임 출력 / FS4 명령 파싱 / FS5 스캔 스케줄러 / FS6 TOOL TX / FS7 cross-ch PING.
- 검증 순서 = **`1차작업_검증계획_5단계.md` Phase 1~3**(FG23↔PC → 감지기→FG23→PC → PC Python UI).

## 5. 주의 (교훈 반복 금지)
- RAIL SoC Empty 기반 필수 / radioconf Save 재생성(assert66) / boot HFXO / Studio↔Dropbox reload(외부 편집 후).
- 스펙 단일 기준: `FG23_UART_인터페이스.md`, `RF_동작_상세정의_WORKING.md`(§7 디코드·§G 채널).

---

## 부록 — Claude Code에게 줄 요청 문장 (복사용)

```
NetAnalyzer_FG23_V01 (Network Analyzer FG23 스니퍼) 펌웨어를 시작한다.
먼저 `Network_Analyzer/작업지시_ClaudeCode_FG23_프로젝트생성.md`를 정독해라.

## 0. 작업 원칙 (필수 — 모든 프로젝트 공통)
0. ★판단 기준(최상위): 모든 판단은 `../JUDGMENT_V1.0.md` 를 최상위로 따른다. 핵심:
   "내 문제로 인식", 안전 절대선(소방 경보·연동 기능 방해 금지 — 스니퍼는 수동 관측 우선),
   규격 근거(KFI 기술기준·시험세칙), 추측보다 실측, 단계적·병행+fallback,
   시험 체크리스트로 회귀 방지. ★응답은 "등불" — 답+리스크 함께 제시, 원칙과 어긋나면 즉시 되묻는다.
1. 데이터 기반 — 추측 금지. RAIL/EFR32 동작은 출처 확인 후 적용(근거 링크 남김).
   Silicon Labs 문서 MCP `silicon-labs-docs`(kapa.ai) 사용. ODROID/Android는 Hardkernel 위키·datasheet 근거.
2. 변경 시 검증 루프: 빌드 → 실측(수신 확인/전류) → 결과 → 다음. 한 번에 한 변수.
3. 코드 변경 시 원본을 주석으로 남긴다.
4. ★`@소장:` 태그 = 소장 직접 편집 = 최우선. 작업 전 `grep -rn "@소장"` 전수 반영 후 태그 삭제.
5. 스텝별 롤백 구조(JUDGMENT §3.11): `#if STEP_N` 가드, known-good 보관, 회귀 시 baseline 복귀+bisection.

## 작업 지시
- 감지기 프로젝트 `LTD_W10D/Software/LTD_W10D_V03`(drv_rf/rf_proto/radioconf)를 참조하되,
  이 장비는 **비배터리 상시전원**이라 저전력 로직(수동 EM2·WOR·TCXO 게이팅)을 쓰지 말고
  **고성능(HFXO 상시 ON · Power Manager EM1까지 · 채널당 상시 RX)** 으로 구성해라.
- 프로젝트 생성·컴포넌트 설치·radioconf 편집 등 Simplicity Studio GUI 작업은 내가(사용자) 하게
  단계별로 안내해주고, 소스/설정 코드는 네가 편집해라.
- 진행: §1 생성 안내 → §2 전원/클럭 고성능 설정 → §3 코드 구조 스캐폴딩 →
  §4의 FS1~FS7을 한 스텝씩(빌드→실측 검증→다음)으로.
- 검증은 `1차작업_검증계획_5단계.md` Phase 1~3(FG23↔PC UART, 감지기→FG23→PC) 기준.
- 스펙 단일 기준 = `FG23_UART_인터페이스.md`, `RF_동작_상세정의_WORKING.md`.

먼저 §1 프로젝트 생성 안내부터 시작해라.
```

> ※ 위 **§0 작업 원칙은 모든 프로젝트(LTD_W10D·FG23·M1S) 공통** — 각 스트림 Claude Code 요청 문장 앞에 동일하게 붙인다. (각 프로젝트 `CLAUDE.md §0`에도 동일 원칙 존재)
