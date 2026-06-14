#!/bin/bash
# Reproducer for minizinc 1249 internal segfault during flatten.
#
# NOTE: this is a DIFFERENT bug from the one in
# ../minizinc-1222-flatten-segfault/ . The 1222 bug (gfd-schedule) is fixed
# in 1249; this bug (peaceable_queens) is a NEW regression introduced in 1249.
#
# Expected output: "MiniZinc error: Memory violation detected (segmentation fault)."
#
# Minimal library trigger: nosets.mzn + fzn_diffn_nonstrict.mzn together.
# Removing either include makes the crash disappear. The model itself does
# NOT use diffn — the crash is triggered purely by the presence of these two
# includes in the loaded library while flattening peaceable_queens.mzn.

set -u
cd "$(dirname "$0")"

# 1249 is the snap revision that regressed. /snap/bin/minizinc points to
# whichever revision is current on the host; we explicitly exercise 1249.
MINIZINC="${MINIZINC:-/snap/minizinc/1249/bin/minizinc}"
if [[ ! -x "$MINIZINC" ]]; then
    MINIZINC=/snap/bin/minizinc
fi

echo "minizinc version:"
"$MINIZINC" --version 2>&1 | head -1
echo

echo "Running -c (compile only, no solver)..."
"$MINIZINC" --solver ./sabori_min.msc -c \
    --fzn /tmp/repro.fzn \
    peaceable_queens.mzn n8_q3.json
ec=$?

echo
echo "exit code: $ec"
if [ $ec -ne 0 ]; then
    echo "REPRODUCED: minizinc crashed during flatten."
else
    echo "NOT REPRODUCED: minizinc completed without crashing."
fi
