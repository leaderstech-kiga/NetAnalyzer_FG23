#!/bin/bash
# FG23 스니퍼 펌웨어 v2.1 프로토콜 구현용 Claude Code 실행 스크립트 (2026-10-22)
# 더블클릭(또는 `bash 이파일`). NetAnalyzer_FG23 에서 Network_Analyzer·NetAnalyzer_App 을 참조로 붙인다.
set -euo pipefail

BASE="/Users/kigagon/Dropbox/InH.Co.Ltd/Project/Ext_Project/Leaders-tech/Git_Kraken_Project/Git_Kraken_Project_Macbook"
FW="$BASE/NetAnalyzer_FG23"          # ← 작업 대상

for d in "$FW" "$BASE/Network_Analyzer" "$BASE/NetAnalyzer_App"; do
  [ -d "$d" ] || { echo "!! 없는 경로: $d"; exit 1; }
done

cd "$FW"

PROMPT='작업지시_ClaudeCode_FG23_v2.1_20261022.md 를 정독하고 §0 문서(특히 Network_Analyzer/FG23_UART_프로토콜_v2.1_확정_20261022.md)를 읽은 뒤 F0 부터 진행해. 빌드·플래시는 내가 Simplicity Studio 로 한다. 각 스텝은 실측 결과를 내가 줄 때까지 커밋하지 말고, 끝나면 보고 후 내 승인을 기다려. 문서끼리 충돌하거나 v2.1 과 다르게 해야 하면 추측하지 말고 멈춰서 질문해.'

exec claude \
  --add-dir "$BASE/Network_Analyzer" \
  --add-dir "$BASE/NetAnalyzer_App" \
  "$PROMPT"
