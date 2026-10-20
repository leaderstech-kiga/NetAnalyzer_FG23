#!/usr/bin/env python3
"""
fg23_window.py — S11a 방전 로그에서 §0.2 "창(window)" 을 뽑는다.

  재는 것:  BAT_CRITICAL 후보전압(기본 9.0V)에 도달한 시각부터
            팩 BMS 차단(기본 8.4V) 또는 로그 종료까지의 **초**
  근거:     작업지시_ClaudeCode_S11a_INA219_20261016.md §5-10
            FG23_UART_프로토콜_v2_확장안_20261014.md §0.2

사용:
    python3 fg23_window.py discharge.log
    python3 fg23_window.py discharge.log --crit 9300 --cut 8400 --csv out.csv

입력 형식 (ina219.c 가 1Hz 로 찍는 줄):
    [S11a] t=123456ms vpack=9004mV vsys=9001mV i=-00931mA shunt=-9310uV (DIS) raw ...

  ※ t= 가 없는 구버전 로그도 읽는다 — 그 경우 [S1] tick=N 을 1초 시계로 쓰고
    **추정값임을 결과에 명시**한다. (tick 간격은 "최소 1초"라 조금씩 길어진다)
"""

import argparse
import re
import sys

# t= 는 선택적 — 구버전 로그 호환
RE_S11A = re.compile(
    r"\[S11a\]\s+"
    r"(?:t=(?P<t>\d+)ms\s+)?"
    r"vpack=(?P<vpack>\d+)mV\s+"
    r"vsys=(?P<vsys>-?\d+)mV\s+"
    r"i=(?P<i>[+-]\d+)mA"
)
RE_TICK = re.compile(r"\[S1\]\s+tick=(?P<n>\d+)")


def parse(path):
    """로그에서 (t_s, vpack_mV, i_mA) 목록과 시계 출처를 뽑는다."""
    rows = []
    last_tick = None
    have_t = False

    # 터미널 로그는 CR/LF 가 섞이고 깨진 바이트가 있을 수 있다 → errors='replace'
    with open(path, "r", encoding="utf-8", errors="replace", newline=None) as f:
        for raw in f:
            for line in raw.replace("\r", "\n").split("\n"):
                m = RE_TICK.search(line)
                if m:
                    last_tick = int(m.group("n"))
                m = RE_S11A.search(line)
                if not m:
                    continue
                if m.group("t") is not None:
                    have_t = True
                    t_s = int(m.group("t")) / 1000.0
                elif last_tick is not None:
                    t_s = float(last_tick)          # 추정 시계
                else:
                    continue                        # 시각을 모르면 버린다
                rows.append((t_s, int(m.group("vpack")), int(m.group("i"))))
    return rows, have_t


def fmt_hms(sec):
    sec = int(round(sec))
    return f"{sec // 3600:d}:{(sec % 3600) // 60:02d}:{sec % 60:02d}"


def main():
    ap = argparse.ArgumentParser(description="S11a 방전 로그 → 창(window) 측정")
    ap.add_argument("log")
    ap.add_argument("--crit", type=int, default=9000,
                    help="BAT_CRITICAL 후보 전압 mV (기본 9000)")
    ap.add_argument("--cut", type=int, default=8400,
                    help="팩 BMS 차단 전압 mV (기본 8400)")
    ap.add_argument("--csv", help="구간 데이터를 CSV 로 저장")
    a = ap.parse_args()

    rows, have_t = parse(a.log)
    if not rows:
        sys.exit("[!] [S11a] 줄을 하나도 못 찾았다 — 로그 형식을 확인하라")

    t0, v0, _ = rows[0]
    tN, vN, iN = rows[-1]

    print(f"파일        : {a.log}")
    print(f"샘플        : {len(rows)} 개")
    print(f"시계        : " + ("t= (펌웨어 ms, 정확)" if have_t
                               else "★[S1] tick 추정 — 오차 누적 가능"))
    print(f"기록 구간   : {fmt_hms(tN - t0)}  ({v0} mV → {vN} mV)")

    # ---- 평균 전류 (부호 그대로. 방전이면 음수) ----
    avg_i = sum(r[2] for r in rows) / len(rows)
    print(f"평균 전류   : {avg_i:+.0f} mA")

    # ---- 임계 도달 / 차단 ----
    crit = next((r for r in rows if r[1] <= a.crit), None)
    if crit is None:
        print(f"\n[!] {a.crit} mV 에 도달하지 않았다. 창을 잴 수 없다.")
        print(f"    최저 전압 = {min(r[1] for r in rows)} mV")
        return

    cut = next((r for r in rows if r[1] <= a.cut), None)
    end = cut if cut else rows[-1]
    reason = (f"{a.cut} mV 도달" if cut else "로그 종료(전원 차단으로 추정)")

    window = end[0] - crit[0]

    print()
    print("=" * 58)
    print(f"  {a.crit} mV 도달   t = {fmt_hms(crit[0])}   (i={crit[2]:+d} mA)")
    print(f"  종료            t = {fmt_hms(end[0])}   ({reason}, {end[1]} mV)")
    print(f"  ★창(window)    = {window:.1f} 초  ({window/60:.2f} 분)")
    print("=" * 58)

    # ---- 구간 기울기 ----
    seg = [r for r in rows if crit[0] <= r[0] <= end[0]]
    if len(seg) >= 2 and window > 0:
        slope = (seg[0][1] - seg[-1][1]) / window
        print(f"  구간 기울기    = {slope:.2f} mV/s")
        # 계측 오차가 시간으로 환산되면 얼마인가 (전압 ±0.5%/±1%)
        for pct, label in ((0.5, "상온"), (1.0, "전온도")):
            err_mv = a.crit * pct / 100.0
            if slope > 0:
                print(f"    계측오차 ±{pct}% ({label}) = ±{err_mv:.0f} mV "
                      f"→ 시간으로 ±{err_mv/slope:.1f} 초")

    # ---- 정전력 보정 안내 ----
    seg_i = [abs(r[2]) for r in seg if r[2] != 0]
    if seg_i:
        imin, imax = min(seg_i), max(seg_i)
        print(f"  구간 전류      = {imin}~{imax} mA")
        if imax < imin * 1.02:
            print("    → 거의 일정. 정저항(저항부하)에 가깝다면 창이 **낙관적**일 수 있다")
        else:
            print("    → 전압 하강에 따라 전류가 변했다. 실부하(정전력) 특성으로 보인다")

    print()
    print("판정 방법:")
    print(f"  창({window:.0f}s) > Android 정상종료 시간 + 여유  →  BAT_CRITICAL "
          f"{a.crit}mV 유지 가능")
    print(f"  창({window:.0f}s) < Android 정상종료 시간         →  ★대기시간을 늘리지 말고 "
          f"BAT_CRITICAL 을 올린다 (--crit 9300 등으로 재계산해 보라)")

    if a.csv:
        with open(a.csv, "w", encoding="utf-8") as f:
            f.write("t_s,vpack_mV,i_mA\n")
            for t, v, i in seg:
                f.write(f"{t:.3f},{v},{i}\n")
        print(f"\nCSV 저장: {a.csv} ({len(seg)} 행)")


if __name__ == "__main__":
    main()
