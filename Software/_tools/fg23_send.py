#!/usr/bin/env python3
"""
FG23 UART 프레임 송신 도구 (PC → FG23)

기준: Network_Analyzer/FG23_UART_인터페이스.md §1
  ST | LEN(3) | TYPE(1) | DATA(540) | DUMMY(8) | CHK(4) | ED   = 560B 고정
  CHK = CRC-16/CCITT-FALSE over LEN+TYPE+DATA+DUMMY (552B)

S4a 완료조건 3종을 그대로 쏜다:
  ① 정상 프레임        → FG23 가 "rx OK"
  ② CRC 를 일부러 깨뜨림 → "rx CRC FAIL calc=.. recv=.."
  ③ 프레임 중간 절단     → 다음 ST 로 재동기 (resync 카운터 증가)

사용:
  python3 fg23_send.py /dev/tty.usbserial-XXXX            # 3종 순차 (기본)
  python3 fg23_send.py /dev/tty.usbserial-XXXX --cmd "CH 3"
  python3 fg23_send.py /dev/tty.usbserial-XXXX --case 1
  python3 fg23_send.py --selftest                          # 시리얼 없이 CRC 자가검증

  포트 찾기(macOS): ls /dev/tty.usb*
  ※ 보율 기본값 921600 (FG23 는 1.5M 가능하나 USB-UART 부품이 1M 상한)
  의존성: pip3 install pyserial
"""
import argparse
import sys
import time

DATA_LEN   = 540
DUMMY_LEN  = 8
TOTAL_LEN  = 560
PREFIX_LEN = 53          # 앞 12필드 + 콤마12 (§1.1)


def crc16_ccitt_false(data: bytes) -> int:
    """poly=0x1021 init=0xFFFF refin/out=false xorout=0 — 검증벡터 0x29B1"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(type_ch: str, payload: str, corrupt_crc: bool = False,
                valid_len: int | None = None) -> bytes:
    """payload(ASCII) 를 DATA 에 좌측정렬·공백패딩해 560B 프레임을 만든다.

    valid_len: LEN 필드에 넣을 **유효 길이**. 생략하면 len(payload).
      ★TYPE='R' CSV 처럼 payload 를 이미 고정폭 패딩해 넘긴 경우에는
        반드시 명시해야 한다 (§1.1: LEN = 앞12필드 53자 + payload_hex 실제분).
        안 그러면 패딩까지 길이에 들어가 LEN 이 틀어진다.
    """
    if len(payload) > DATA_LEN:
        raise ValueError(f"payload {len(payload)}B > DATA {DATA_LEN}B")

    n     = len(payload) if valid_len is None else valid_len
    data  = payload.ljust(DATA_LEN)
    ln    = f"{n:03d}"
    dummy = " " * DUMMY_LEN

    scope = (ln + type_ch + data + dummy).encode("ascii")
    assert len(scope) == 552, len(scope)

    crc = crc16_ccitt_false(scope)
    if corrupt_crc:
        crc ^= 0x0001                      # 1비트만 뒤집어 확실히 불일치

    frame = ("ST" + ln + type_ch + data + dummy + f"{crc:04X}" + "ED").encode("ascii")
    assert len(frame) == TOTAL_LEN, len(frame)
    return frame


def decode_frame(buf: bytes) -> str:
    """수신 560B 프레임을 검증·해독해 한 줄 요약. 실패 사유도 문자열로 낸다."""
    if len(buf) != TOTAL_LEN:
        return f"길이 {len(buf)}B (기대 560)"
    if buf[:2] != b"ST":
        return "ST 없음"
    if buf[-2:] != b"ED":
        return "ED 없음"

    scope = buf[2:2 + 552]
    calc  = crc16_ccitt_false(scope)
    try:
        recv = int(buf[TOTAL_LEN - 6:TOTAL_LEN - 2].decode("ascii"), 16)
    except ValueError:
        return "CHK 비-hex"
    if calc != recv:
        return f"CRC 불일치 calc={calc:04X} recv={recv:04X}"

    try:
        ln = int(buf[2:5].decode("ascii"))
    except ValueError:
        return "LEN 비-숫자"

    typ  = chr(buf[5])
    data = buf[6:6 + DATA_LEN].decode("ascii", "replace")
    return f"CRC OK  type={typ} len={ln:03d}  DATA=[{data[:ln]}]"


def recv_frame(sp, timeout_s: float = 1.0):
    """ST 로 시작하는 560B 를 모아 반환. 못 모으면 (None, 원시바이트)."""
    import time as _t
    buf = bytearray()
    t0 = _t.time()
    while _t.time() - t0 < timeout_s:
        chunk = sp.read(sp.in_waiting or 1)
        if chunk:
            buf.extend(chunk)
            i = buf.find(b"ST")
            if i >= 0 and len(buf) - i >= TOTAL_LEN:
                return bytes(buf[i:i + TOTAL_LEN]), bytes(buf)
        else:
            _t.sleep(0.02)
    return None, bytes(buf)


def selftest() -> bool:
    ok = True

    v = crc16_ccitt_false(b"123456789")
    print(f"  CRC(\"123456789\") = {v:04X} (exp 29B1) {'OK' if v == 0x29B1 else 'NG'}")
    ok &= (v == 0x29B1)

    v = crc16_ccitt_false(b"ABC")
    print(f"  CRC(\"ABC\")       = {v:04X} (exp F508) {'OK' if v == 0xF508 else 'NG'}")
    ok &= (v == 0xF508)

    # 스펙 §1.2 예제 프레임 (TYPE='R') 재현 → CHK 는 EB7D 여야 한다
    fields = ["0000128374", "003", "-071", "210", "OK", "02", "01",
              "05", "01", "0123", "04", "008", "0102030405060708"]
    widths = [10, 3, 4, 3, 4, 2, 2, 2, 2, 4, 2, 3, 486]
    padded = [f.ljust(w) for f, w in zip(fields, widths)]
    valid  = ",".join(padded[:12]) + "," + fields[12]      # LEN 은 패딩 전 유효분
    frame  = build_frame("R", ",".join(padded), valid_len=len(valid))
    chk    = frame[TOTAL_LEN - 6:TOTAL_LEN - 2].decode()
    print(f"  예제 프레임 LEN  = {frame[2:5].decode()} (exp 069) "
          f"{'OK' if frame[2:5] == b'069' else 'NG'}  [유효 {len(valid)}자]")
    print(f"  예제 프레임 CHK  = {chk} (exp EB7D) {'OK' if chk == 'EB7D' else 'NG'}")
    ok &= (chk == "EB7D") and (frame[2:5] == b"069")

    return bool(ok)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="시리얼 포트 (예: /dev/tty.usbserial-0001)")
    ap.add_argument("--baud", type=int, default=921600,
                    help="기본 921600 (2026-07-31 상향). 이전 값은 115200")
    # ★기본값을 "VER" 로 둔 이유: "STAT" 은 문자열 안에 **ST** 가 있어서
    #   재동기가 가짜 프레임 머리로 잡는다(오프셋 6). 실제로 있을 수 있는
    #   상황이라 시험 가치는 있지만, 기본값이면 결과 해석이 어려워진다.
    #   → 가짜 동기 스트레스 시험은 `--cmd STAT` 로 **의도적으로** 돌린다.
    ap.add_argument("--cmd", default="VER", help="TYPE='C' DATA 내용 "
                    "(STAT 을 주면 ST 가 payload 에 섞여 가짜 동기 시험이 된다)")
    ap.add_argument("--case", type=int, choices=[1, 2, 3],
                    help="1=정상 2=CRC깨짐 3=중간절단 (생략 시 1,2,3 순차)")
    ap.add_argument("--s4b", action="store_true",
                    help="S4b 시험: VER/STAT/TX/CH/XYZ 를 보내고 ACK 를 해독·검증")
    ap.add_argument("--s4c", action="store_true",
                    help="S4c 시험: CH/PHY/MODE/SCAN 반영 + STAT 되읽기 + 범위밖 거절")
    ap.add_argument("--selftest", action="store_true", help="시리얼 없이 CRC 자가검증만")
    args = ap.parse_args()

    print("== CRC 자가검증 ==")
    if not selftest():
        print("!! 자가검증 실패 — 스크립트 자체가 스펙과 어긋남. 전송 중단.")
        return 1
    if args.selftest:
        return 0

    if not args.port:
        ap.error("port 를 지정하거나 --selftest 를 쓰세요")

    try:
        import serial                              # type: ignore
    except ImportError:
        print("!! pyserial 없음 → pip3 install pyserial")
        return 1

    cases = [args.case] if args.case else [1, 2, 3]

    with serial.Serial(args.port, args.baud, timeout=0.2) as sp:
        time.sleep(0.3)
        sp.reset_input_buffer()

        if args.s4c:
            # (명령, 기대 result, detail 에 반드시 들어가야 할 문자열 or None)
            # ★순서 의존: 상태가 누적되므로 되읽기(STAT)로 반영을 확인한다.
            trials = [
                ("CH 3",          "OK",  "ch=003 phy=short"),
                ("STAT",          "OK",  "ch=003 phy=short"),   # 되읽기
                ("PHY long",      "ERR", "conflicts"),          # ch=3 과 모순
                ("PHY short",     "OK",  "phy=short"),          # ch=3 과 일치
                ("CH 103",        "OK",  "ch=103 phy=long"),    # wake → phy 자동
                ("PHY short",     "ERR", "conflicts"),          # ch=103 과 모순
                ("CH 50",         "ERR", "invalid"),            # 없는 채널
                ("CH 200",        "ERR", "invalid"),            # 범위 밖
                ("CH abc",        "ERR", "invalid"),            # 비숫자
                ("MODE active",   "OK",  "mode=active"),
                ("MODE bogus",    "ERR", "invalid"),
                ("MODE passive",  "OK",  "mode=passive"),       # 안전측 복귀
                ("SCAN 1 2 3",    "OK",  "scan_n=3"),
                ("SCAN 1 2 3 4 5", "ERR", "too many"),
                ("SCAN 50",       "ERR", "invalid"),
                ("STAT",          "OK",  "ch=103 phy=long mode=passive scan_n=3"),
            ]
            npass = 0
            for cmd, want, must in trials:
                sp.reset_input_buffer()
                sp.write(build_frame("C", cmd)); sp.flush()
                frame, raw = recv_frame(sp, 1.5)
                if frame is None:
                    print(f"  {cmd:<16} -> ACK 없음 (원시 {len(raw)}B)")
                    continue

                info = decode_frame(frame)
                got, detail = "?", ""
                if "CRC OK" in info and "DATA=[" in info:
                    body = info.split("DATA=[", 1)[1].rstrip("]")
                    parts = body.split(",", 2)
                    if len(parts) >= 3:
                        got, detail = parts[1].strip(), parts[2].strip()

                ok = (got == want) and (must is None or must in detail)
                npass += ok
                mark = "PASS" if ok else "FAIL"
                print(f"  {cmd:<16} -> {mark} [{got}] {detail}")
                if not ok:
                    print(f"      기대: result={want} detail 에 '{must}' 포함")

            print(f"\n  S4c 결과: {npass}/{len(trials)} PASS")
            return 0 if npass == len(trials) else 1

        if args.s4b:
            # S4b: 명령별 ACK 왕복. 기대 result 를 같이 적어 자동 판정한다.
            # ★2026-07-31 수정: "CH 3" 기대값을 ERR -> OK 로.
            #   이 세트는 FS_S4=2(S4c 미구현) 때 작성돼 CH 가 "not_yet" 을 낼 걸로
            #   기대했다. FS_S4=3 부터는 CH 가 정상 동작하는 게 맞다.
            #   → 펌웨어 버그가 아니라 **시험 기대값이 의도된 변경을 못 따라간 오경보**.
            #   교훈: 기능을 넣으면 기존 회귀 세트의 "미구현 기대"도 같이 손봐야 한다.
            trials = [("VER",    "OK"),      # 조회 — 펌웨어 버전
                      ("STAT",   "OK"),      # 조회 — 현재 상태
                      ("TX 1 short AABB", "ERR"),   # ★안전: S9 까지 거절돼야 함
                      ("CH 3",   "OK"),      # S4c 구현됨 (FS_S4>=3)
                      ("XYZZY",  "ERR")]     # 미지 명령
            npass = 0
            for cmd, want in trials:
                sp.reset_input_buffer()
                sp.write(build_frame("C", cmd)); sp.flush()
                frame, raw = recv_frame(sp, 1.5)

                if frame is None:
                    print(f"  {cmd:<18} -> ACK 없음  (원시 {len(raw)}B)")
                    continue

                info = decode_frame(frame)
                got  = "?"
                if "CRC OK" in info and "DATA=[" in info:
                    body = info.split("DATA=[", 1)[1].rstrip("]")
                    parts = body.split(",")
                    if len(parts) >= 2:
                        got = parts[1].strip()
                ok = (got == want)
                npass += ok
                print(f"  {cmd:<18} -> {'PASS' if ok else 'FAIL'} "
                      f"(기대 {want}, 실제 {got})\n      {info}")

            print(f"\n  S4b 결과: {npass}/{len(trials)} PASS")
            return 0 if npass == len(trials) else 1

        for c in cases:
            if c == 1:
                f, desc = build_frame("C", args.cmd), "정상 프레임"
            elif c == 2:
                f, desc = build_frame("C", args.cmd, corrupt_crc=True), "CRC 1비트 손상"
            else:
                # 중간 절단 후 곧바로 정상 프레임 → 재동기가 되면 뒤엣것이 rx OK 로 잡혀야 한다
                f = build_frame("C", args.cmd)[:300] + build_frame("C", args.cmd)
                desc = "300B 에서 절단 + 정상 프레임 연속"

            print(f"\n-- case {c}: {desc} ({len(f)}B 송신) --")
            sp.write(f)
            sp.flush()

            time.sleep(0.6)                        # FG23 응답/로그 수집
            out = sp.read(sp.in_waiting or 1)
            if out:
                sys.stdout.write(out.decode("ascii", "replace"))
            else:
                print("  (수신 없음)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
