#!/usr/bin/env bash
# Diagnostic modes are independent MPI jobs. Formal T3 is never replaced.
set -uo pipefail
task_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$task_root" || exit 2
device_home=${DEVICE_HOME:-/usr/local/musa}
device_arch=${MUSA_ARCH:-mp_31}
output="$task_root/test/unittest/device_api/build/ir_batch_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$output" || exit 2
export LD_LIBRARY_PATH="$task_root/build/lib:$device_home/lib:$device_home/lib64:${LD_LIBRARY_PATH:-}"
unset FLAGCX_DEBUG_FILE FLAGCX_MUSA_K1_DIAG
printf 'case\tstatus\texit\tlog\n' > "$output/results.tsv"
printf 'root=%s\ndevice_home=%s\narch=%s\nranks=8\nsize=1K..16M\n' \
  "$task_root" "$device_home" "$device_arch" > "$output/environment.txt"
git rev-parse HEAD >> "$output/environment.txt" 2>/dev/null || true

archive_results() {
  python3 - "$output" <<'PY'
from pathlib import Path
import sys, zipfile
root = Path(sys.argv[1])
with zipfile.ZipFile(root / 'diagnostics.zip', 'w', compression=zipfile.ZIP_DEFLATED) as z:
    for file in sorted(root.iterdir()):
        if file.is_file() and file.suffix in ('.log', '.tsv', '.txt'):
            z.write(file, file.name)
print('DOWNLOAD=' + str(root / 'diagnostics.zip'))
PY
}
trap archive_results EXIT
echo "OUTPUT=$output"

timeout --kill-after=5s 600s make -B -C test/kernel/musa device_ir.o \
  PROJECT_ROOT="$task_root" USE_MUSA=1 COMPILE_KERNEL=1 \
  DEVICE_HOME="$device_home" MUSA_ARCH="$device_arch" \
  > "$output/kernel-build.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  printf 'kernel-build\tFAIL\t%s\tkernel-build.log\n' "$rc" >> "$output/results.tsv"
  tail -60 "$output/kernel-build.log"
  exit 2
fi
timeout --kill-after=5s 300s make -C test/unittest/device_api -f musa_ir_diagnostic.mk musa-ir-diag \
  "$task_root/test/unittest/device_api/build/bin/test_device_ir_unified_intra" \
  PROJECT_ROOT="$task_root" USE_MUSA=1 COMPILE_KERNEL=1 \
  DEVICE_HOME="$device_home" MUSA_ARCH="$device_arch" \
  > "$output/host-build.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  printf 'host-build\tFAIL\t%s\thost-build.log\n' "$rc" >> "$output/results.tsv"
  tail -60 "$output/host-build.log"
  exit 2
fi

mpi=(mpirun --allow-run-as-root --bind-to none --tag-output -np 8
  -x LD_LIBRARY_PATH
  -x FLAGCX_USE_HETERO_COMM=1 -x FLAGCX_MEM_ENABLE=1
  -x FLAGCX_VMM_ENABLE=0 -x FLAGCX_IB_DISABLE=1 -x FLAGCX_P2P_DISABLE=0
  -x FLAGCX_DEBUG=INFO -x FLAGCX_DEBUG_SUBSYS=ALL)
labels=(read_comm read_net_contexts typed_value_intra typed_pointer_intra ir_intra ir_world)
diagnostic_failures=0
for mode in 1 2 3 4 5 6; do
  label="${labels[$((mode - 1))]}"
  echo "RUN diagnostic $mode: $label"
  timeout --signal=TERM --kill-after=5s 180s \
    "${mpi[@]}" -x "FLAGCX_MUSA_IR_CASE=$mode" \
    "$task_root/test/unittest/device_api/build/bin/test_musa_ir_diagnostic" \
    -b 1K -e 16M -f 2 > "$output/$label.log" 2>&1
  rc=$?
  status=FAIL
  if [ "$rc" -eq 0 ] && grep -q "IR DIAGNOSTIC CASE $mode: PASS" "$output/$label.log"; then
    status=PASS
  else
    diagnostic_failures=$((diagnostic_failures + 1))
  fi
  printf '%s\t%s\t%s\t%s.log\n' "$label" "$status" "$rc" "$label" >> "$output/results.tsv"
  echo "$label: $status exit=$rc"
done

# Even if a diagnostic fails, run the untouched formal suite in a fresh job.
# Repeat only successful formal runs; a failure already invalidates acceptance.
formal_passes=0
for round in 1 2 3; do
  label="formal_t3_round$round"
  echo "RUN $label (8 ranks, 1K..16M)"
  timeout --signal=TERM --kill-after=60s 30m \
    "${mpi[@]}" -x FLAGCX_MUSA_IR_DIAG=1 \
    "$task_root/test/unittest/device_api/build/bin/test_device_ir_unified_intra" \
    -b 1K -e 16M -f 2 > "$output/$label.log" 2>&1
  rc=$?
  status=FAIL
  pass_count=$(grep -c 'Final Result: ALL PASS' "$output/$label.log" || true)
  if [ "$rc" -eq 0 ] && [ "$pass_count" -eq 1 ] &&
      ! grep -Eq 'S(1[6-9]|2[0-5]).*FAIL' "$output/$label.log"; then
    status=PASS
    formal_passes=$((formal_passes + 1))
  fi
  printf '%s\t%s\t%s\t%s.log\n' "$label" "$status" "$rc" "$label" >> "$output/results.tsv"
  echo "$label: $status exit=$rc"
  if [ "$status" != PASS ]; then break; fi
done
printf 'Diagnostic failures=%s/6\nFormal T3 passed rounds=%s/3\n' \
  "$diagnostic_failures" "$formal_passes" | tee "$output/summary.txt"
cat "$output/results.tsv"
if [ "$diagnostic_failures" -ne 0 ] || [ "$formal_passes" -ne 3 ]; then exit 1; fi
