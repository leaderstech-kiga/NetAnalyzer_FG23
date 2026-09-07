#!/usr/bin/env python3
"""
FG23 스니퍼 수신 패킷 해독기 (RF v1.1 / VER=0x02)

hex 를 눈으로 읽는 대신 바로 "무슨 일이 일어났는지" 를 출력한다.

기준: LTD_W10D_V03/rf_proto.h  (단일 기준 — 여기가 바뀌면 이 파일도 고친다)
  헤더 :160~167 / MSG_TYPE :114~152 / 페이로드 :186~257
  상수 :32(VER) :37(HDR_LEN) :78~80(GROUP) :194~198(flags)
       :244~245(센티넬) :267(FLOOD_MAX_HOPS)
문서: ../../스니퍼_패킷_해독_가이드.md  (같은 내용의 사람용 판)

와이어 배치 (RAIL 은 wire 그대로 준다 → buf[0] 이 LEN):
  off 0    1    2    3    4    5     6     7    8...
     LEN  VER  GRP  SRC  MSG  SEQlo SEQhi HOP  payload
  ※ LEN 은 헤더+payload 길이. 로그의 len= 은 LEN+1.
  ※ SEQ 는 little-endian.

사용:
  # 1) hex 한 줄 바로 해독
  python3 fg23_decode.py --hex "08 02 01 03 01 10 00 04 02"

  # 2) 터미널에서 복사한 로그 붙여넣기 (여러 패킷 한꺼번에)
  python3 fg23_decode.py --log capture.txt
  pbpaste | python3 fg23_decode.py            # stdin (macOS)

  # 3) 시리얼에 붙어 실시간 해독  ★현장에서 이걸 쓴다
  python3 fg23_decode.py --port /dev/tty.usbserial-0001

  # 4) 도구 자가검증 (시리얼 없이)  ※RSSI 판정은 링크 마진 기준 (rf_proto.h:279~283)
  python3 fg23_decode.py --selftest

  포트 찾기(macOS): ls /dev/tty.usb*
  보율 기본 921600 / 의존성(3번만): pip3 install pyserial
"""
import argparse
import re
import sys

# ── 상수 (rf_proto.h) ────────────────────────────────────────────────────────
PROTO_VER      = 0x02        # :32
HDR_LEN        = 7           # :37
GROUP_MIN      = 1           # :78
GROUP_MAX      = 24          # :79
FLOOD_MAX_HOPS = 4           # :267
SENTINEL_U16   = 0xFFFF      # :244
SENTINEL_TEMP  = -32768      # :245  (0x8000)

# 링크 마진 (:279~283) — 판정은 RSSI 절대값이 아니라 마진으로 한다.
#   마진 = RSSI - SENSITIVITY  (rf_proto.h:204)
# ⚠ SENSITIVITY 는 실측이 아니라 보수적 추정치다 (소스 주석 "실측 보정 필요").
#   아래 판정 전부가 이 미검증 전제 위에 있다. 실측으로 보정되면 여기를 고친다.
SENSITIVITY_DBM   = -110     # :281
MARGIN_PASS_DB    = 10       # :282
MARGIN_WARN_DB    = 5        # :283
# 상한은 프로젝트 규격에 없다 — 일반 RF 통념(포화 영역).
SATURATION_DBM    = -10

NODE_SPECIAL = {240: "수신기(RS-485 게이트웨이)",
                241: "외부전송(TCP/IP·CDMA)",
                242: "점검장비(TOOL)"}

CAUSE = {0x00: "SMOKE 연기 자체감지",
         0x01: "HEAT 정온식 자체감지",
         0x02: "MANUAL KEY 수동시험",
         0x03: "COMBINED 복합",
         0x04: "EMR 유선연동 수신"}

FLAGS = [(0x01, "LOW_BAT 배터리부족"),
         (0x02, "SENSOR_ERR 센서고장"),
         (0x04, "TEST_MODE 시험모드"),
         (0x08, "LINK_WARN 마진5~10dB"),
         (0x10, "COMM_FAULT 24h미수신")]

# MSG: 이름, 영역, 송신채널(g=데이터/w=wake)
MSG = {
    0x01: ("ALARM_FIRE",         "알람", "w"), 0x02: ("ALARM_STOP",     "알람", "w"),
    0x03: ("ALARM_PRE",          "알람", "w"),
    0x10: ("HEARTBEAT",          "상태", "g"), 0x11: ("STATUS_REPORT",  "상태", "g"),
    0x12: ("BATTERY_LOW",        "상태", "g"), 0x13: ("SENSOR_ERROR",   "상태", "g"),
    0x14: ("TAMPER",             "상태", "g"), 0x15: ("LINK_QUALITY",   "상태", "g"),
    0x16: ("NEIGHBOR_REPORT",    "상태", "g"), 0x18: ("HELLO",          "상태", "g"),
    0x20: ("TEST_REQ",           "점검", "g"), 0x21: ("TEST_ACK",       "점검", "g"),
    0x22: ("SURVEY_REQ",         "점검", "g"), 0x23: ("SURVEY_REPLY",   "점검", "g"),
    0x24: ("LINK_PING",          "점검", "w"), 0x25: ("LINK_PONG",      "점검", "g"),
    0x30: ("RECEIVER_QUERY",     "명령", "w"), 0x31: ("RECEIVER_REPLY", "명령", "g"),
    0x32: ("RECEIVER_BROADCAST", "명령", "g"), 0x33: ("CMD_SILENCE",    "명령", "w"),
    0x34: ("CMD_TEST",           "명령", "w"), 0x35: ("CMD_RESET",      "명령", "w"),
    0x36: ("EXT_QUERY",          "명령", "w"), 0x37: ("EXT_GROUP_REPORT","명령","g"),
    0xF0: ("DEBUG_PING",         "디버그", "g"), 0xF1: ("DEBUG_RSSI",   "디버그", "g"),
}


# ── 원시값 도우미 ───────────────────────────────────────────────────────────
def u16(b, off):
    return b[off] | (b[off + 1] << 8)


def i16(b, off):
    v = u16(b, off)
    return v - 0x10000 if v & 0x8000 else v


def temp_str(v):
    if v == SENTINEL_TEMP:
        return "센티넬(미측정)"
    return f"{v / 10:.1f} C"


def u16_str(v, unit=""):
    if v == SENTINEL_U16:
        return "센티넬(미측정/미배포)"
    return f"{v}{unit}"


def rssi_verdict(r):
    """RSSI(dBm) -> (마진dB, 표시문자열). 규격 근거 rf_proto.h:279~283."""
    margin = r - SENSITIVITY_DBM
    if r >= SATURATION_DBM:
        return margin, f"마진 {margin}dB  <- ⚠ 포화 의심(너무 가까움)"
    if margin >= MARGIN_PASS_DB:
        return margin, f"마진 {margin}dB  PASS"
    if margin >= MARGIN_WARN_DB:
        return margin, f"마진 {margin}dB  <- ⚠ WARN (감지기가 LINK_WARN 세움)"
    return margin, f"마진 {margin}dB  <- ❌ 불량 (PASS 기준 {MARGIN_PASS_DB}dB)"


def flags_str(v):
    if v == 0:
        return "0x00 (이상 없음)"
    on = [n for m, n in FLAGS if v & m]
    unknown = v & ~sum(m for m, _ in FLAGS)
    if unknown:
        on.append(f"미정의비트 0x{unknown:02X}")
    return f"0x{v:02X} = " + " | ".join(on)


# ── 페이로드 해독기 ─────────────────────────────────────────────────────────
def dec_status(p, warn):
    """STATUS_REPORT(0x11) / RECEIVER_REPLY(0x31) 16B — rf_proto.h:246~257"""
    if len(p) < 16:
        warn.append(f"STATUS_REPORT payload 가 16B 여야 하는데 {len(p)}B")
        return []
    bat, fl = u16(p, 0), p[2]
    smoke, temp = u16(p, 3), i16(p, 5)
    on_s, on_t = u16(p, 7), i16(p, 9)
    t_thr, s_thr, cfg = u16(p, 11), u16(p, 13), p[15]

    if bat != SENTINEL_U16 and bat < 2400:
        warn.append(f"배터리 낮음 {bat}mV")
    if fl & 0x02:
        warn.append("SENSOR_ERR 세팅 — 알람 신뢰도 낮음")

    return [("bat_mv", u16_str(bat, " mV")),
            ("flags", flags_str(fl)),
            ("smoke_raw", u16_str(smoke)),
            ("temp_c10", temp_str(temp)),
            ("onset_smoke", u16_str(on_s) + "   <- 화재 판정 순간"),
            ("onset_temp", temp_str(on_t) + "   <- 화재 판정 순간"),
            ("set_temp_thr", u16_str(t_thr)),
            ("set_smoke_thr", u16_str(s_thr)),
            ("cfg_ver", f"{cfg}" + (" (default)" if cfg == 0 else ""))]


def dec_bat_flags(p, warn):
    if len(p) < 3:
        warn.append(f"payload 3B 여야 하는데 {len(p)}B")
        return []
    return [("bat_mv", u16_str(u16(p, 0), " mV")), ("flags", flags_str(p[2]))]


def dec_linkq(p, warn):
    if len(p) < 3:
        return []
    r = p[0] - 256 if p[0] & 0x80 else p[0]
    out = [("rssi", f"{r} dBm"), ("lqi", str(p[1])), ("margin", f"{p[2]} dB")]
    if len(p) >= 4:
        out.append(("ngb_cnt", str(p[3])))
    return out


def dec_pong(p, warn):
    if len(p) < 2:
        return []
    r = p[0] - 256 if p[0] & 0x80 else p[0]
    return [("rx_rssi", f"{r} dBm"), ("seq", f"{p[1]}  <- 응답 대상 PING 의 seq")]


PAYLOAD = {
    0x11: dec_status, 0x31: dec_status,
    0x10: dec_bat_flags, 0x21: dec_bat_flags,
    0x15: dec_linkq, 0x23: dec_linkq,
    0x25: dec_pong,
}


# ── 패킷 해독 ───────────────────────────────────────────────────────────────
class Decoder:
    def __init__(self):
        self.seen = {}      # (src, seq) -> 최초 hop

    def decode(self, raw: bytes, meta: dict | None = None):
        meta = meta or {}
        warn, note = [], []

        if len(raw) < 1 + HDR_LEN:
            return {"error": f"너무 짧다 ({len(raw)}B). 최소 {1+HDR_LEN}B 필요", "raw": raw}

        ln, ver, grp, src, msg = raw[0], raw[1], raw[2], raw[3], raw[4]
        seq = raw[5] | (raw[6] << 8)
        hop = raw[7]
        payload = raw[8:]

        # 길이 정합
        if ln != HDR_LEN + len(payload):
            warn.append(f"LEN 불일치: LEN={ln} 인데 실제 헤더+payload={HDR_LEN+len(payload)}")
        # 헤더 sanity (가이드 §7)
        if ver != PROTO_VER:
            warn.append(f"VER 이 0x{ver:02X} (기대 0x{PROTO_VER:02X}) — 오수신/구버전 의심")
        if not (GROUP_MIN <= grp <= GROUP_MAX):
            warn.append(f"GROUP {grp} 가 범위 밖 ({GROUP_MIN}~{GROUP_MAX})")
        if src == 0:
            warn.append("SRC=0 — Setting mode 는 발신 금지인데 송신됨 (감지기 버그 의심)")
        elif not (1 <= src <= 61 or src in NODE_SPECIAL):
            warn.append(f"SRC {src} 가 정의 범위 밖 (1~61 또는 240~242)")

        name, area, ch_kind = MSG.get(msg, (f"미정의(0x{msg:02X})", "?", "?"))
        if msg not in MSG:
            warn.append(f"MSG 0x{msg:02X} 는 rf_proto.h 에 없다")

        # HOP → 원본/중계본
        is_alarm = msg <= 0x0F
        if msg in (0x24, 0x25):
            origin = "PING/PONG (중계 대상 아님)" if hop == 1 else f"HOP={hop} (PING/PONG 은 1 이 정상)"
        elif hop == FLOOD_MAX_HOPS:
            origin = "원본"
        elif is_alarm and hop < FLOOD_MAX_HOPS:
            origin = f"중계본 ({FLOOD_MAX_HOPS - hop}회 중계됨)"
        else:
            origin = f"HOP={hop}"

        # 중복(SRC,SEQ) 추적 — 같은 사건인지
        key = (src, seq)
        if key in self.seen:
            note.append(f"★같은 (SRC={src}, SEQ={seq}) 를 이미 봤다 (그때 HOP={self.seen[key]}) "
                        f"→ 새 사건이 아니라 같은 사건의 중계본")
        else:
            self.seen[key] = hop

        # payload
        fields = []
        if msg in (0x01, 0x03) and payload:
            c = payload[0]
            fields.append(("cause", f"0x{c:02X} {CAUSE.get(c, '미정의')}"))
        elif msg == 0x12 and len(payload) >= 2:
            fields.append(("bat_mv", u16_str(u16(payload, 0), " mV")))
        elif msg == 0x13 and payload:
            fields.append(("err_code", f"0x{payload[0]:02X}"))
        elif msg in (0x33, 0x35):
            fields.append(("passcode", payload.hex(" ").upper() or "(없음)"))
        elif msg in PAYLOAD:
            fields += PAYLOAD[msg](payload, warn)
        elif payload:
            fields.append(("payload", payload.hex(" ").upper()))

        # 교차검증: cause 와 onset 이 모순인가 (가이드 §7)
        if msg == 0x01 and payload:
            c = payload[0]
            st = self._last_status
            if st and st[0] == src:
                _, on_s, on_t, thr_t = st
                if c == 0x01 and on_t != SENTINEL_TEMP and on_t < 600:
                    warn.append(f"cause=HEAT 인데 같은 노드의 onset_temp 가 {on_t/10:.1f}C "
                                f"(상온) — 오작동 또는 임계 오류 의심")
                if c == 0x00 and on_s == 0:
                    warn.append("cause=SMOKE 인데 onset_smoke=0 — 오작동 또는 센서 이상 의심")
        if msg in (0x11, 0x31) and len(payload) >= 16:
            self._last_status = (src, u16(payload, 7), i16(payload, 9), u16(payload, 11))

        # 채널 정합 (스니퍼가 들은 채널 vs 규격상 송신 채널)
        rx_ch = meta.get("ch")
        if rx_ch is not None and ch_kind in ("g", "w"):
            expect = (100 + grp) if ch_kind == "w" else grp
            if rx_ch != expect:
                note.append(f"규격상 이 프레임은 ch{expect} 로 나간다 (수신 ch{rx_ch}). "
                            f"ch{grp}/ch{100+grp} 는 같은 주파수라 교차 수신될 수 있다")

        return {"len": ln, "ver": ver, "grp": grp, "src": src, "msg": msg,
                "name": name, "area": area, "seq": seq, "hop": hop, "origin": origin,
                "fields": fields, "warn": warn, "note": note, "raw": raw, "meta": meta}

    _last_status = None


# ── 출력 ────────────────────────────────────────────────────────────────────
def render(d) -> str:
    if "error" in d:
        return f"  !! 해독 불가: {d['error']}\n     raw: {d['raw'].hex(' ').upper()}"

    m, out = d["meta"], []
    head = f"#{m['idx']} " if "idx" in m else ""
    tail = []
    if "ch" in m:
        tail.append(f"ch={m['ch']}")
    if "rssi" in m:
        _, verdict = rssi_verdict(m["rssi"])
        tail.append(f"rssi={m['rssi']}dBm ({verdict})")
    if "crc" in m:
        tail.append(f"crc={m['crc']}")
    out.append(f"{head}{' '.join(tail)}")
    out.append(f"  {d['raw'].hex(' ').upper()}")
    out.append(f"  ├ {d['name']} (0x{d['msg']:02X}, {d['area']}영역)")

    src = d["src"]
    src_s = f"{src}" + (f" [{NODE_SPECIAL[src]}]" if src in NODE_SPECIAL else "")
    out.append(f"  ├ GROUP {d['grp']} / SRC {src_s} / SEQ {d['seq']} / HOP {d['hop']} = {d['origin']}")

    for k, v in d["fields"]:
        out.append(f"  │   {k:<14}= {v}")
    for n in d["note"]:
        out.append(f"  ├ note: {n}")
    for w in d["warn"]:
        out.append(f"  └ ⚠ {w}")
    if not d["warn"]:
        out.append("  └ 헤더 이상 없음")
    return "\n".join(out)


# ── 입력 파싱 ───────────────────────────────────────────────────────────────
RE_HDR = re.compile(r"#(\d+)\s+ch=(\d+)\s+rssi=(-?\d+)\s+crc=(\w+)\s+len=(\d+)")
RE_DMP = re.compile(r"^\s*(?:\[\w+\]\s*)?([0-9A-Fa-f]{4}):\s*((?:[0-9A-Fa-f]{2}[ \t]*)+)")


def parse_log(lines):
    """스니퍼 로그에서 (bytes, meta) 를 뽑는다. 헤더줄 없이 덤프만 있어도 동작."""
    out, meta, buf, have = [], None, bytearray(), False

    def flush():
        nonlocal meta, buf, have
        if have and buf:
            out.append((bytes(buf), meta or {}))
        meta, buf, have = None, bytearray(), False

    for ln in lines:
        h = RE_HDR.search(ln)
        if h:
            flush()
            meta = {"idx": int(h.group(1)), "ch": int(h.group(2)),
                    "rssi": int(h.group(3)), "crc": h.group(4), "len": int(h.group(5))}
            have = True
            continue
        d = RE_DMP.match(ln)
        if d:
            off = int(d.group(1), 16)
            if off == 0 and buf:      # 헤더줄 없이 새 패킷이 시작된 경우
                flush()
            have = True
            buf += bytes.fromhex(d.group(2).replace("\t", " ").strip().replace(" ", ""))
    flush()
    return out


def parse_hex(s):
    s = re.sub(r"0x", "", s, flags=re.I)
    s = re.sub(r"[^0-9A-Fa-f]", "", s)
    if len(s) % 2:
        raise ValueError("hex 자릿수가 홀수다")
    return bytes.fromhex(s)


# ── 자가검증 ────────────────────────────────────────────────────────────────
SELFTEST_LOG = """
[S6a] #9 ch=101 rssi=-12 crc=PASS len=9
[S6a]   0000: 08 02 01 03 01 10 00 04 02
[S6a] #10 ch=101 rssi=-11 crc=PASS len=24
[S6a]   0000: 17 02 01 03 11 11 00 04 0A 0C 00 00 00 F3 00 00
[S6a]   0016: 00 F2 00 FF FF FF FF 00
[S6a] #11 ch=101 rssi=-12 crc=PASS len=8
[S6a]   0000: 07 02 01 03 02 12 00 04
[S6a] #12 ch=101 rssi=-18 crc=PASS len=9
[S6a]   0000: 08 02 01 03 01 10 00 03 02
"""


def selftest():
    """2026-09-04 실측 캡처로 파서·해독기를 검증한다.
    ※ 도구 자체도 자가검증이 필요하다 (fg23_send.py 가 자기 LEN 버그를 이렇게 잡았다)."""
    ok = fail = 0

    def chk(label, got, exp):
        nonlocal ok, fail
        if got == exp:
            ok += 1
            print(f"  PASS  {label}")
        else:
            fail += 1
            print(f"  FAIL  {label}: got={got!r} exp={exp!r}")

    pk = parse_log(SELFTEST_LOG.splitlines())
    chk("로그에서 패킷 4개 추출", len(pk), 4)
    chk("#10 재조립 24B (2줄 이어붙임)", len(pk[1][0]), 24)
    chk("#10 meta len 과 실제 일치", pk[1][1]["len"], len(pk[1][0]))

    dec = Decoder()
    d = [dec.decode(b, m) for b, m in pk]

    chk("#9  ALARM_FIRE", d[0]["name"], "ALARM_FIRE")
    chk("#9  SEQ little-endian = 16", d[0]["seq"], 16)
    chk("#9  원본 판정", d[0]["origin"], "원본")
    chk("#9  cause=MANUAL", d[0]["fields"][0][1], "0x02 MANUAL KEY 수동시험")
    chk("#9  헤더 경고 없음", d[0]["warn"], [])

    f10 = dict(d[1]["fields"])
    chk("#10 STATUS_REPORT", d[1]["name"], "STATUS_REPORT")
    chk("#10 bat_mv 3082", f10["bat_mv"], "3082 mV")
    chk("#10 temp 24.3C", f10["temp_c10"], "24.3 C")
    chk("#10 onset_temp 24.2C", f10["onset_temp"].split("   ")[0], "24.2 C")
    chk("#10 임계 센티넬", f10["set_temp_thr"], "센티넬(미측정/미배포)")
    chk("#10 flags 정상", f10["flags"], "0x00 (이상 없음)")
    chk("#10 ch1 대상인데 ch101 수신 → note", any("교차 수신" in n for n in d[1]["note"]), True)

    chk("#11 ALARM_STOP", d[2]["name"], "ALARM_STOP")
    chk("#11 payload 없음", d[2]["fields"], [])

    chk("#12 중계본 판정", d[3]["origin"], "중계본 (1회 중계됨)")
    chk("#12 같은 사건 note", any("새 사건이 아니라" in n for n in d[3]["note"]), True)

    # 이상 프레임 합성 — 경고가 실제로 뜨는지 (무효한 방어코드 방지)
    bad = bytes([0x07, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x04])
    b = Decoder().decode(bad)
    chk("VER 이상 경고", any("VER" in w for w in b["warn"]), True)
    chk("GROUP 범위 경고", any("GROUP" in w for w in b["warn"]), True)
    chk("SRC=0 경고", any("SRC=0" in w for w in b["warn"]), True)

    # RSSI 링크 마진 판정 (rf_proto.h:281~283) — 규격 경계값
    chk("rssi -86 -> 마진 24dB PASS", rssi_verdict(-86), (24, "마진 24dB  PASS"))
    chk("rssi -100 경계 PASS", rssi_verdict(-100)[0], MARGIN_PASS_DB)
    chk("rssi -101 -> WARN", "WARN" in rssi_verdict(-101)[1], True)
    chk("rssi -105 경계 WARN", rssi_verdict(-105)[0], MARGIN_WARN_DB)
    chk("rssi -106 -> 불량", "불량" in rssi_verdict(-106)[1], True)
    chk("rssi -6 -> 포화", "포화" in rssi_verdict(-6)[1], True)

    short = Decoder().decode(b"\x03\x02\x01")
    chk("짧은 프레임 거절", "error" in short, True)

    lenbad = Decoder().decode(bytes([0x20, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x04]))
    chk("LEN 불일치 경고", any("LEN 불일치" in w for w in lenbad["warn"]), True)

    print(f"\n  결과: {ok} PASS / {fail} FAIL")
    return 0 if fail == 0 else 1


# ── main ────────────────────────────────────────────────────────────────────
def run_stream(port, baud):
    try:
        import serial
    except ImportError:
        print("pyserial 이 없다:  pip3 install pyserial", file=sys.stderr)
        return 2
    dec = Decoder()
    print(f"# {port} @ {baud} — 수신 대기 (Ctrl-C 로 종료)\n")
    buf, meta, acc = "", None, bytearray()
    with serial.Serial(port, baud, timeout=0.2) as sp:
        try:
            while True:
                chunk = sp.read(4096).decode("utf-8", "replace")
                if not chunk:
                    continue
                buf += chunk
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    h = RE_HDR.search(line)
                    if h:
                        if meta and acc:
                            print(render(dec.decode(bytes(acc), meta)) + "\n")
                        meta = {"idx": int(h.group(1)), "ch": int(h.group(2)),
                                "rssi": int(h.group(3)), "crc": h.group(4),
                                "len": int(h.group(5))}
                        acc = bytearray()
                        continue
                    d = RE_DMP.match(line)
                    if d and meta is not None:
                        acc += bytes.fromhex(d.group(2).strip().replace(" ", ""))
                        if len(acc) >= meta["len"]:
                            print(render(dec.decode(bytes(acc), meta)) + "\n")
                            meta, acc = None, bytearray()
        except KeyboardInterrupt:
            print("\n# 종료")
    return 0


def main():
    ap = argparse.ArgumentParser(description="FG23 스니퍼 패킷 해독기")
    ap.add_argument("logfile", nargs="?", help="로그 파일 (없으면 stdin)")
    ap.add_argument("--hex", help='패킷 hex 한 줄 (예: "08 02 01 03 01 10 00 04 02")')
    ap.add_argument("--log", help="로그 파일 (logfile 인자와 동일)")
    ap.add_argument("--port", help="시리얼 포트 — 실시간 해독")
    ap.add_argument("--baud", type=int, default=921600, help="보율 (기본 921600)")
    ap.add_argument("--selftest", action="store_true", help="시리얼 없이 자가검증")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if a.port:
        return run_stream(a.port, a.baud)

    dec = Decoder()
    if a.hex:
        print(render(dec.decode(parse_hex(a.hex))))
        return 0

    src = a.log or a.logfile
    lines = open(src, encoding="utf-8", errors="replace").readlines() if src \
        else sys.stdin.readlines()
    pk = parse_log(lines)
    if not pk:
        print("해독할 패킷을 못 찾았다. --hex 로 직접 넣거나 로그 형식을 확인할 것.",
              file=sys.stderr)
        return 1
    for b, m in pk:
        print(render(dec.decode(b, m)) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
