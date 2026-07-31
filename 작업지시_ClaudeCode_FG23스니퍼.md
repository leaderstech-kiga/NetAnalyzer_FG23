# 작업지시 (Claude Code) — ② FG23 스니퍼 펌웨어

> **프로젝트**: **`NetAnalyzer_FG23_V01`** (`Software/NetAnalyzer_FG23_V01`). 생성·전원설정은 `작업지시_ClaudeCode_FG23_프로젝트생성.md` 선행.
> **대상**: Network Analyzer의 RF 모듈 **A3125LT/EFR32FG23** 스니퍼 펌웨어(비배터리=고성능·상시RX). Simplicity Studio(RAIL) / Claude Code 코드 편집.
> **역할**: 447 v1.1 트래픽 **수동 수신 → CSV 프레임 UART 출력** + **스캔** + **TOOL 능동 TX**. (감지기 아님 — 중계·판정 안 함)
> **기준 스펙**: `FG23_UART_인터페이스.md`(프레임·CSV·CRC·스캔·TX), `RF_동작_상세정의_WORKING.md`(§7 디코드·§G 채널).
> **재활용**: LTD_W10D `drv_rf.c`·`rf_proto.c/h`(RX 방향)·radioconf(2채널 dual-PHY) — 감지기 검증 자산.
> **원칙**: 한 스텝씩 빌드→실측→다음 / `#if` 가드 / 원본 주석 보존 / RAIL은 `silicon-labs-docs` MCP 확인.

---

## 규칙
- 빌드=Simplicity Studio, radioconf=Studio Save 재생성(assert66 회피). Claude Code=코드.
- 검증 도구: 감지기(실트래픽 송신원) / 오실로 / M1S(또는 UART 터미널로 프레임 육안).
- **안전**: 기본 passive(RX). active TX는 MODE active + 명시. 알람 중 TX 자제.

## FS1 — radioconf 2채널 + RX 기본
- radioconf: 데이터 0~24(짧은PA) + wake 100~124(롱PA) 2그룹(감지기와 동일 재사용).
- RX 초기화(RAIL), 채널/PHY 설정 API.
- **검증**: 감지기 송신 → FG23 수신 로그(RAILtest 스타일), CRC pass.

## FS2 — v1.1 파서 → CSV 13필드
- 수신 프레임 → 헤더(ver/group/src/msg_type/seq/hop) 파싱 + 메타(ts_ms/ch/rssi/lqi/crc) → **CSV 13필드**(§FG23 §1.1 고정 자리수).
- payload_hex = RF DATA 원본(감지기 디코드는 M1S 몫, FG23는 hex 그대로).
- **검증**: 알려진 감지기 프레임 → CSV 필드 정확.

## FS3 — UART 프레임 출력 (RX, TYPE='R')
- **고정 560B 프레임**: `ST|LEN(3)|R|DATA(540)|DUMMY(8)|CRC16(4)|ED`, 115200 8-N-1.
- **CRC-16/CCITT-FALSE**(자기검증 0x29B1) — `FG23_UART §1.2` C코드.
- **검증**: M1S(또는 터미널)에서 프레임 수신·CRC 검증, 예제 CRC=EB7D 재현.

## FS4 — M1S→FG23 명령 파싱 (TYPE='C')
- `CH n`·`PHY short|long`·`SCAN <ch1..chN>`·`MODE passive|active` 파싱 + ACK(TYPE='A').
- **검증**: M1S에서 명령 → FG23 반영·ACK.

## FS5 — 스캔 스케줄러 (경량)
- 앱이 준 **1~4채널 목록**을 타이머로 순환(camp=1채널 상주). **wake 재방문 ≤ 4초**(알람 보장), 알람 우선.
- **검증**: 다채널 스캔 중 데이터(짧은PA)·wake(롱PA 알람) 각각 수신, 재방문 주기 측정.

## FS6 — TOOL 능동 TX (active 모드)
- `TX <ch> <phy> <app_frame_hex>` → **length+CRC 래핑 후 송신**(app_frame은 M1S 구성). ACK.
- 커버: SURVEY_REQ·LINK_PING·RECEIVER_QUERY·EXT_QUERY·CFG_SET·CMD_SILENCE/RESET.
- **검증**: FG23 TX → 감지기 수신·응답 → FG23 RX → M1S 확인.

## FS7 — cross-channel PING 지원 (C5 측정)
- 대상 채널 지정 PING 송신(브리지 도달성 측정). `TX <타채널> long <ping>`.
- **검증**: 타 채널 그룹 PONG 수신·RSSI.

## 통합 검증
- 감지기 그룹 실트래픽 → FG23 전 msg_type CSV 디코드 → M1S 대조.
- RAILtest 스니퍼/감지기 로그와 교차검증(디코드 정확도).

## 미결(스펙측)
- payload_hex 실제 최대(RF_MAX_FRAME) 재확인, TX 명령 문법 세부.
