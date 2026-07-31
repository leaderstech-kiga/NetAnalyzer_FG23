#!/usr/bin/env python3
"""
FG23 수신 상태기 시뮬레이터 — 펌웨어 `uart_frame.c` 의 rx_feed/rx_resync 재현

★왜 두나: 2026-07-31 S4a 실측에서 이 시뮬레이터의 예측이 실기 출력과
  **완전히 일치**했다(BAD_ED → BAD_ED → rx OK, resync 카운트까지).
  이후 상태기를 손댈 때 여기서 먼저 돌려 예상 출력을 뽑고 실측과 대조한다.
  → 실측이 예측과 다르면 그 자체가 버그 신호. (추측 대신 대조)

  ※ 펌웨어를 고치면 **이 파일도 같이 고쳐야** 대조가 성립한다.

사용:
  python3 fg23_rxsim.py             # 강화 전/후 비교
  python3 fg23_rxsim.py --cmd VER
"""
import argparse
import importlib.util
import os

TOTAL, SCOPE = 560, 552
VALID_TYPES = "RCA"


def _load_sender():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fg23_send.py")
    spec = importlib.util.spec_from_file_location("fg23_send", p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def simulate(stream: bytes, hdr_precheck: bool):
    """(events, resync_total) 반환. hdr_precheck = 2026-07-31 헤더 사전검사 강화."""
    buf, state, resync, events, consumed = bytearray(), 0, 0, [], 0
    m = _load_sender()

    def hdr_ok(p) -> bool:
        """p[0..5] = 'S','T', LEN(숫자3), TYPE(R/C/A). 6B 이상 확보 전제."""
        return (p[0] == ord('S') and p[1] == ord('T')
                and all(chr(p[i]).isdigit() for i in (2, 3, 4))
                and chr(p[5]) in VALID_TYPES)

    def do_resync():
        nonlocal buf, state, resync
        resync += 1
        j = 1
        while j < len(buf):
            if buf[j] == ord('S'):
                avail = len(buf) - j
                if avail >= 6:
                    # ★강화: 후보 판정 시점에 헤더 패턴까지 본다.
                    #   (idx==6 검사만 두면 재동기 직후엔 버퍼가 이미 커서 무효)
                    if not hdr_precheck or hdr_ok(buf[j:j + 6]):
                        break
                elif avail < 2 or buf[j + 1] == ord('T'):
                    break
            j += 1
        if j >= len(buf):
            buf, state = bytearray(), 0
        else:
            buf = buf[j:]
            state = 2 if len(buf) >= 2 else 1

    for b in stream:
        consumed += 1
        if state == 0:
            if b == ord('S'):
                buf, state = bytearray([b]), 1
            continue
        if state == 1:
            if b == ord('T'):
                buf.append(b); state = 2
            elif b == ord('S'):
                buf = bytearray([b])
            else:
                buf, state = bytearray(), 0
            continue

        buf.append(b)

        # ★헤더 사전검사: ST 다음은 LEN(숫자 3) + TYPE(R/C/A)
        if hdr_precheck and len(buf) == 6:
            ok = all(chr(buf[i]).isdigit() for i in (2, 3, 4)) \
                 and chr(buf[5]) in VALID_TYPES
            if not ok:
                events.append((consumed, "BAD_HDR", resync + 1))
                do_resync()
                continue

        if len(buf) < TOTAL:
            continue

        ed = buf[-2:] == b'ED'
        calc = m.crc16_ccitt_false(bytes(buf[2:2 + SCOPE]))
        try:
            recv = int(buf[TOTAL - 6:TOTAL - 2].decode(), 16)
        except ValueError:
            recv = -1

        if ed and calc == recv:
            events.append((consumed, f"rx OK len={buf[2:5].decode()}", resync))
            buf, state = bytearray(), 0
        else:
            events.append((consumed, "CRC FAIL" if ed else "BAD_ED", resync + 1))
            do_resync()

    return events, resync


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", default="STAT",
                    help="payload (기본 STAT — 안에 ST 가 있어 가짜 동기를 유발)")
    args = ap.parse_args()

    m = _load_sender()
    stream = m.build_frame("C", args.cmd)[:300] + m.build_frame("C", args.cmd)
    print(f"시나리오: '{args.cmd}' 300B 절단 + 정상 프레임 ({len(stream)}B)\n")

    for label, pre in (("강화 전 (헤더 사전검사 없음)", False),
                       ("강화 후 (ST + LEN숫자3 + TYPE)", True)):
        ev, tot = simulate(stream, pre)
        print(f"-- {label}")
        for consumed, what, r in ev:
            print(f"     [{consumed:4d}B 소비] {what:<16} resync={r}")
        print(f"     → 총 재동기 {tot}회, 복구까지 {ev[-1][0]}B 소비\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
