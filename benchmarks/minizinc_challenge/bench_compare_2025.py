#!/usr/bin/env python3
"""Sabori vs CP-SAT ベンチマーク比較スクリプト (MiniZinc Challenge 2025 problems)

後方互換ラッパー。実体は bench_compare.py に統合済み。
"""
import sys
from bench_compare import cleanup_stale_processes, run_year

if __name__ == "__main__":
    cleanup_stale_processes()
    run_year("2025")
