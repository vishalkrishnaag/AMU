#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CASES="${1:-training/common_memory/test_cases.tsv}"

if [[ "$CASES" =~ ^[0-9]+$ ]]; then
  CASES="training/common_memory/test_cases.tsv"
fi

cd "$ROOT"

if [[ ! -f "$CASES" ]]; then
  echo "missing test case file: $CASES" >&2
  exit 2
fi

if [[ ! -x ./intense.out ]]; then
  make
fi

POET="$ROOT/training/poet_algorithm_v1.in10s"
OUT="$ROOT/training/common_memory/generated_outputs/latest"
RUNNERS="$OUT/runners"
CANDIDATES="$OUT/candidates"
TRACES="$OUT/traces"
PROPOSED="$OUT/proposed_poet"

rm -rf "$OUT"
mkdir -p "$RUNNERS" "$CANDIDATES" "$TRACES" "$PROPOSED"
cp "$POET" "$PROPOSED/poet_algorithm_v1_current.in10s"

escape_in10s() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\r'/}"
  value="${value//$'\n'/\\n}"
  printf '%s' "$value"
}

safe_name() {
  printf '%s' "$1" | tr -c 'A-Za-z0-9_-' '_'
}

REPORT="$OUT/report.tsv"
EXAMPLES="$OUT/training_examples.tsv"
SUMMARY="$OUT/summary.txt"

printf 'status\tname\texpected\tactual\troute\tproblem\n' > "$REPORT"
printf 'problem\texpected\tactual\troute\tstatus\n' > "$EXAMPLES"

total=0
passed=0

while IFS=$'\t' read -r name problem expected; do
  [[ -z "${name:-}" ]] && continue
  [[ "$name" == \#* ]] && continue

  total=$((total + 1))
  id="$(safe_name "$name")"
  runner="$RUNNERS/${id}.in10s"
  candidate="$CANDIDATES/${id}_candidate.in10s"
  poet_trace="$TRACES/${id}_poet.txt"
  candidate_trace="$TRACES/${id}_candidate.txt"

  escaped_problem="$(escape_in10s "$problem")"
  escaped_candidate="$(escape_in10s "$candidate")"

  cat > "$runner" <<EOF_RUNNER
IMPORT $POET

main:
    SETARG 0 "$escaped_problem"
    SETARG 1 "$escaped_candidate"
    SETARG 2 "auto"
    CALL poet_solve
    PRINT
    RET
EOF_RUNNER

  if ./intense.out "$runner" main 8 > "$poet_trace" 2>&1 && [[ -f "$candidate" ]]; then
    ./intense.out "$candidate" main 4 > "$candidate_trace" 2>&1 || true
    actual="$(tr -d '\r' < "$candidate_trace" | tail -n 1)"
  else
    actual="<poet_failed>"
  fi

  route="$(head -n 1 "$poet_trace" | sed -n 's/.*"route":"\([^"]*\)".*/\1/p')"
  [[ -z "$route" ]] && route="unknown"

  if [[ "$actual" == "$expected" ]]; then
    status="PASS"
    passed=$((passed + 1))
  else
    status="FAIL"
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$status" "$name" "$expected" "$actual" "$route" "$problem" >> "$REPORT"
  printf '%s\t%s\t%s\t%s\t%s\n' "$problem" "$expected" "$actual" "$route" "$status" >> "$EXAMPLES"
done < "$CASES"

failed=$((total - passed))
if [[ "$total" -eq 0 ]]; then
  accuracy="0"
else
  accuracy="$(awk -v p="$passed" -v t="$total" 'BEGIN { printf "%.4f", p / t }')"
fi

{
  echo "cases: $total"
  echo "passed: $passed"
  echo "failed: $failed"
  echo "accuracy: $accuracy"
  echo "outputs: $OUT"
} > "$SUMMARY"

cat "$SUMMARY"
