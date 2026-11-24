#!/usr/bin/env bash
#
# mayhem/test.sh — runs nnstreamer's own gtest unit tests (built by mayhem/build.sh in
# mayhem/build-test with normal flags) through `meson test`. exit 0 = pass. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

B="$SRC/mayhem/build-test"
[ -f "$B/build.ninja" ] || { echo "test build missing: run mayhem/build.sh" >&2; exit 1; }
# Not run (skipped): they fail in a container regardless of the code under test —
# unittest_rate/unittest_plugins/unittest_filter_custom/unittest_datareposink rely on timing or
# files in the source tree that the in-image run does not provide, unittest_src_iio needs a
# writable fake sysfs layout, and unittest_datareposrc exceeds meson's 30 s timeout.
SKIP=" unittest_rate unittest_plugins unittest_src_iio unittest_datareposink unittest_filter_custom unittest_datareposrc "
mapfile -t ALL < <(meson test -C "$B" --list 2>/dev/null | sed 's/^.* \/ //')
RUN=(); skipped=0
for t in "${ALL[@]}"; do
  case "$SKIP" in *" $t "*) skipped=$((skipped+1)) ;; *) RUN+=("$t") ;; esac
done
[ "${#RUN[@]}" -gt 0 ] || { echo "no tests found in $B" >&2; exit 1; }
LOG=$(mktemp)
meson test -C "$B" --no-rebuild --num-processes "$MAYHEM_JOBS" --print-errorlogs "${RUN[@]}" 2>&1 | tee "$LOG" | tail -20
# A test passes only if meson saw it succeed AND gtest itself reported "[  PASSED  ]" with no
# "[  FAILED  ]" line (an exit status alone is not trusted).
read -r ok fail < <(python3 -I - "$B/meson-logs/testlog.json" <<'PY'
import json, sys
ok = fail = 0
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    t = json.loads(line)
    out = t.get("stdout") or ""
    if t.get("result") == "OK" and "[  PASSED  ]" in out and "[  FAILED  ]" not in out:
        ok += 1
    else:
        fail += 1
print(ok, fail)
PY
)
ok=${ok:-0}; fail=${fail:-0}
# Tests that did not report a result count as failures.
missing=$(( ${#RUN[@]} - ok - fail )); [ "$missing" -gt 0 ] && fail=$((fail + missing))
rm -f "$LOG"
emit_ctrf "meson-gtest" "$ok" "$fail" "$skipped"
