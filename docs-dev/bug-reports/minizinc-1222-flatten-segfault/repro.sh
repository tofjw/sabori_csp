#!/bin/bash
# Reproducer for minizinc 1222 internal segfault during flatten.
#
# Expected output: "MiniZinc error: Memory violation detected (segmentation fault)."
#
# Removing ANY one of the four includes in library/redefinitions.mzn makes
# the crash disappear. The crash happens during the flatten phase, before
# any solver executable is invoked.

set -u
cd "$(dirname "$0")"

MINIZINC="${MINIZINC:-/snap/bin/minizinc}"

echo "minizinc version:"
"$MINIZINC" --version 2>&1 | head -1
echo

echo "Running -c (compile only, no solver)..."
"$MINIZINC" --solver ./sabori_min.msc -c \
    --fzn /tmp/repro.fzn \
    gfd-schedule2.mzn n25f5d20m10k3.dzn
ec=$?

echo
echo "exit code: $ec"
if [ $ec -ne 0 ]; then
    echo "REPRODUCED: minizinc crashed during flatten."
else
    echo "NOT REPRODUCED: minizinc completed without crashing."
fi
