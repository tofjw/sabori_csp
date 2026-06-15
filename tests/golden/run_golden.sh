#!/usr/bin/env bash
# ゴールデンマスター: 直接実行可能な全 fzn の `-a -s` 出力(全解 + 決定論的統計)を
# 記録し、リファクタ前後で挙動が完全一致するかを機械判定する。
#
# 使い方:
#   tests/golden/run_golden.sh record   # 現行バイナリで期待値を生成/更新
#   tests/golden/run_golden.sh check    # 期待値と一致するか検証 (既定)。audit も実行
#   tests/golden/run_golden.sh list     # 対象 fzn 一覧を再構築 (Error 系は要 excluded 登録)
#   tests/golden/run_golden.sh audit    # 全 fzn が corpus か excluded のどちらかに属すか監査
#
# 捕捉内容: 解出力(stdout) + `% Stats:` / `% NG length` (stderr)。時刻は含めない=決定論的。
#
# 死テスト化の防止: tests/fzn/constraints/ 配下の全 .fzn は corpus.txt(byte照合) か
# excluded.txt(理由付き除外) の いずれか に必ず属さねばならない。どちらにも無い fzn
# (新規追加・Error 退行で corpus から漏れたもの) は audit が大声で失敗する。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${FZN_SABORI:-$ROOT/build/src/fzn/fzn_sabori}"
SRC="$ROOT/tests/fzn/constraints"
EXP="$ROOT/tests/golden/expected"
CORPUS="$ROOT/tests/golden/corpus.txt"
EXCLUDED="$ROOT/tests/golden/excluded.txt"
TIMEOUT=15

# excluded.txt から ROOT 相対パスだけを抽出 (コメント/空行/行内 # 以降を除去)
excluded_paths() {
    [ -f "$EXCLUDED" ] || return 0
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$EXCLUDED" | grep -v '^[[:space:]]*$'
}

# 監査: 全 .fzn が corpus か excluded のどちらかに属すか検証。orphan があれば 1 を返す。
audit_corpus() {
    local orphans=0 f rel
    for f in $(find "$SRC" -name '*.fzn' | sort); do
        rel="${f#$ROOT/}"
        if grep -qxF "$rel" "$CORPUS" 2>/dev/null; then continue; fi
        if excluded_paths | grep -qxF "$rel"; then continue; fi
        if [ "$orphans" -eq 0 ]; then
            echo "AUDIT FAIL: corpus.txt にも excluded.txt にも無い fzn (死テスト化の疑い):"
        fi
        orphans=$((orphans+1))
        local probe; probe="$(timeout "$TIMEOUT" "$BIN" -a "$rel" 2>&1 | head -1)"
        echo "  $rel  ||  ${probe:0:70}"
    done
    if [ "$orphans" -gt 0 ]; then
        echo "→ $orphans 件。corpus 再構築(record)するか、Error/不安定なら excluded.txt に理由付きで登録すること。"
        return 1
    fi
    echo "AUDIT OK: 全 $(find "$SRC" -name '*.fzn' | wc -l) fzn が corpus または excluded に属す"
    return 0
}

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
    local f rel probe dropped=0 unack=0
    for f in $(find "$SRC" -name '*.fzn' | sort); do
        rel="${f#$ROOT/}"
        probe="$(timeout "$TIMEOUT" "$BIN" -a "$f" 2>&1 | head -1)"
        case "$probe" in
            Error:*)                             # 未サポート/書式エラーは byte 照合から除外…
                dropped=$((dropped+1))           # …が黙殺せず必ず報告する
                if excluded_paths | grep -qxF "$rel"; then
                    echo "  [excluded] $rel  ||  ${probe:0:60}"
                else
                    unack=$((unack+1))
                    echo "  [UNACK!! ] $rel  ||  ${probe:0:60}"
                fi
                ;;
            *) echo "$rel" >> "$CORPUS" ;;
        esac
    done
    echo "corpus: $(wc -l < "$CORPUS") fzn / Error 除外 $dropped 件 (うち未登録 $unack 件)"
    if [ "$unack" -gt 0 ]; then
        echo "→ 上記 [UNACK!!] は excluded.txt 未登録。意図的なら理由付きで登録、不具合なら fzn を修正すること。"
        return 1
    fi
    return 0
}

cmd="${1:-check}"
case "$cmd" in
  list) build_corpus ;;
  audit)
    [ -s "$CORPUS" ] || { echo "corpus.txt なし。先に record/list を実行"; exit 2; }
    audit_corpus || exit 1
    ;;
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
    # まず監査: 全 fzn が corpus/excluded に属すか (死テスト化の検出)
    audit_corpus || exit 1
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
  *) echo "usage: run_golden.sh [record|check|list|audit]"; exit 2 ;;
esac
