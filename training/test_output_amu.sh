#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CASES="${1:-training/common_memory/test_cases.tsv}"

if [[ "$CASES" =~ ^[0-9]+$ ]]; then
  CASES="training/common_memory/test_cases.tsv"
fi

cd "$ROOT"
training/run_amu_epoch.sh "$CASES"

REPORT="training/common_memory/generated_outputs/latest/report.tsv"
SUMMARY="training/common_memory/generated_outputs/latest/summary.txt"

if [[ ! -f "$REPORT" ]]; then
  echo "missing report: $REPORT" >&2
  exit 2
fi

failures="$(awk -F '\t' 'NR > 1 && $1 != "PASS" { count++ } END { print count + 0 }' "$REPORT")"

echo
cat "$SUMMARY"
echo
column -t -s $'\t' "$REPORT" 2>/dev/null || cat "$REPORT"

if [[ "$failures" -ne 0 ]]; then
  echo
  echo "poet output test failed: $failures failing case(s)" >&2
  exit 1
fi

echo
echo "poet output test passed"
