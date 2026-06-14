#!/usr/bin/env bash
# ゴールデンマスター: 直接実行可能な全 fzn の `-a -s` 出力(全解 + 決定論的統計)を
# 記録し、リファクタ前後で挙動が完全一致するかを機械判定する。
#
# 使い方:
#   tests/golden/run_golden.sh record   # 現行バイナリで期待値を生成/更新
#   tests/golden/run_golden.sh check    # 期待値と一致するか検証 (既定)
#   tests/golden/run_golden.sh list     # 対象 fzn 一覧を再構築 (Error 系を除外)
#
# 捕捉内容: 解出力(stdout) + `% Stats:` / `% NG length` (stderr)。時刻は含めない=決定論的。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${FZN_SABORI:-$ROOT/build/src/fzn/fzn_sabori}"
SRC="$ROOT/tests/fzn/constraints"
EXP="$ROOT/tests/golden/expected"
CORPUS="$ROOT/tests/golden/corpus.txt"
TIMEOUT=15

run_one() {  # $1=fzn  -> 解出力 + 統計 (決定論的部分のみ)
    local f="$1" out err
    out="$(timeout "$TIMEOUT" "$BIN" -a -s "$f" 2>/tmp/golden_err.$$)"
    err="$(grep -E '^% (Stats:|NG length)' /tmp/golden_err.$$)"
    rm -f /tmp/golden_err.$$
    printf '%s\n--- stats ---\n%s\n' "$out" "$err"
}

key() {  # fzn path -> expected ファイル名
    local rel="${1#$SRC/}"; echo "${rel%.fzn}" | tr '/' '__'
}

build_corpus() {
    : > "$CORPUS"
    local f probe
    for f in $(find "$SRC" -name '*.fzn' | sort); do
        probe="$(timeout "$TIMEOUT" "$BIN" -a "$f" 2>&1 | head -1)"
        case "$probe" in
            Error:*) ;;                          # 未サポート/書式エラーは除外
            *) echo "${f#$ROOT/}" >> "$CORPUS" ;;
        esac
    done
    echo "corpus: $(wc -l < "$CORPUS") fzn (Error 系除外)"
}

cmd="${1:-check}"
case "$cmd" in
  list) build_corpus ;;
  record)
    [ -s "$CORPUS" ] || build_corpus
    mkdir -p "$EXP"; rm -f "$EXP"/*.txt
    while read -r rel; do
        run_one "$ROOT/$rel" > "$EXP/$(key "$ROOT/$rel").txt"
    done < "$CORPUS"
    echo "recorded $(ls "$EXP"/*.txt | wc -l) golden files -> $EXP"
    ;;
  check)
    [ -s "$CORPUS" ] || { echo "corpus.txt なし。先に record を実行"; exit 2; }
    pass=0; fail=0; failed=""
    while read -r rel; do
        k="$(key "$ROOT/$rel")"
        if diff -q "$EXP/$k.txt" <(run_one "$ROOT/$rel") >/dev/null 2>&1; then
            pass=$((pass+1))
        else
            fail=$((fail+1)); failed="$failed $rel"
        fi
    done < "$CORPUS"
    echo "GOLDEN: pass=$pass fail=$fail"
    if [ "$fail" -gt 0 ]; then
        echo "FAILED fzn:"; for x in $failed; do echo "  $x"; done
        echo "差分確認: diff $EXP/<key>.txt <($BIN -a -s <fzn>)"
        exit 1
    fi
    echo "ALL GREEN"
    ;;
  *) echo "usage: run_golden.sh [record|check|list]"; exit 2 ;;
esac
