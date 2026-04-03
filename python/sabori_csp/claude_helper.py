"""Helper utilities for using sabori_csp from Claude Code.

Provides visualization functions, verbose output parsing, and
timeout-aware solving for integration with Claude Code's AI assistant.
"""
from __future__ import annotations

import io
import os
import re
import sys
import threading
from typing import Any, Callable, Sequence

from sabori_csp._expressions import IntVar, LinearExpr
from sabori_csp._model import CpModel
from sabori_csp._solver import CpSolver, SolveStatus


# ---------------------------------------------------------------------------
# Visualization
# ---------------------------------------------------------------------------


def format_grid(
    solution: dict[str, int],
    var_pattern: str,
    rows: int,
    cols: int,
    cell_width: int = 3,
    block_rows: int | None = None,
    block_cols: int | None = None,
    value_map: dict[int, str] | None = None,
) -> str:
    """Render a 2-D grid (Sudoku, Latin square, N-Queens, etc.).

    Parameters
    ----------
    solution : dict mapping variable names to integer values.
    var_pattern : format string with ``{r}`` and ``{c}`` placeholders,
        e.g. ``"cell_{r}_{c}"``.  Indices are 0-based.
    rows, cols : grid dimensions.
    cell_width : width of each cell in characters.
    block_rows, block_cols : if set, draw thicker separators every
        *block_rows* rows / *block_cols* columns (e.g. 3 for Sudoku).
    value_map : optional mapping from integer values to display strings.
    """
    br = block_rows or rows
    bc = block_cols or cols

    def _cell(r: int, c: int) -> str:
        key = var_pattern.format(r=r, c=c)
        val = solution.get(key)
        if val is None:
            return "?".center(cell_width)
        s = value_map[val] if value_map and val in value_map else str(val)
        return s.center(cell_width)

    def _hsep(thick: bool) -> str:
        ch = "=" if thick else "-"
        parts: list[str] = []
        for bc_idx in range(0, cols, bc):
            end = min(bc_idx + bc, cols)
            parts.append(ch * (cell_width * (end - bc_idx) + (end - bc_idx - 1)))
        return "+" + ("+".join(parts)) + "+"

    lines: list[str] = []
    lines.append(_hsep(True))

    for r in range(rows):
        row_parts: list[str] = []
        for bc_idx in range(0, cols, bc):
            end = min(bc_idx + bc, cols)
            cells = [_cell(r, c) for c in range(bc_idx, end)]
            row_parts.append("|".join(cells))
        lines.append("|" + "|".join(row_parts) + "|")

        if r + 1 < rows:
            thick = (r + 1) % br == 0
            lines.append(_hsep(thick))

    lines.append(_hsep(True))
    return "\n".join(lines)


def format_schedule(
    solution: dict[str, int],
    tasks: Sequence[dict[str, Any]],
    width: int = 60,
) -> str:
    """Render an ASCII Gantt chart.

    Parameters
    ----------
    solution : dict mapping variable names to integer values.
    tasks : list of dicts, each with keys:

        - ``"name"`` : display name
        - ``"start_var"`` : variable name for start time
        - ``"duration"`` : integer duration (or variable name)
    width : character width of the timeline area.
    """
    entries: list[tuple[str, int, int]] = []
    for t in tasks:
        start = solution[t["start_var"]]
        dur = t["duration"]
        if isinstance(dur, str):
            dur = solution[dur]
        entries.append((t["name"], start, dur))

    if not entries:
        return "(no tasks)"

    horizon = max(s + d for _, s, d in entries)
    if horizon == 0:
        horizon = 1
    name_width = max(len(name) for name, _, _ in entries)
    scale = width / horizon

    lines: list[str] = []
    # header
    header_label = " " * name_width + "  "
    ticks = ""
    step = max(1, horizon // 10)
    for t in range(0, horizon + 1, step):
        pos = int(t * scale)
        label = str(t)
        ticks += " " * (pos - len(ticks)) + label
    lines.append(header_label + ticks)

    for name, start, dur in entries:
        bar_start = int(start * scale)
        bar_end = int((start + dur) * scale)
        bar_len = max(1, bar_end - bar_start)
        line = "." * bar_start + "=" * bar_len + "." * (width - bar_start - bar_len)
        interval = f"[{start}, {start + dur})"
        lines.append(f"{name:<{name_width}}  |{line}| {interval}")

    return "\n".join(lines)


def format_table(
    solution: dict[str, int],
    var_names: Sequence[str] | None = None,
    headers: tuple[str, str] = ("Variable", "Value"),
) -> str:
    """Render a simple two-column table of variable assignments.

    Parameters
    ----------
    solution : dict mapping variable names to integer values.
    var_names : variables to show (default: all, sorted).
    headers : column header labels.
    """
    if var_names is None:
        var_names = sorted(solution.keys())

    if not var_names:
        return "(no variables)"

    name_w = max(len(headers[0]), max(len(n) for n in var_names))
    val_w = max(
        len(headers[1]),
        max(len(str(solution.get(n, "?"))) for n in var_names),
    )

    sep = f"+-{'-' * name_w}-+-{'-' * val_w}-+"
    hdr = f"| {headers[0]:<{name_w}} | {headers[1]:>{val_w}} |"

    lines = [sep, hdr, sep]
    for n in var_names:
        v = solution.get(n, "?")
        lines.append(f"| {n:<{name_w}} | {str(v):>{val_w}} |")
    lines.append(sep)
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Verbose output parsing & diagnostics
# ---------------------------------------------------------------------------

_RE_PRESOLVE_START = re.compile(
    r"presolve start: (\d+) constraints, (\d+) variables"
    r" \(avg ([\d.]+) constraints/var, max (\d+)\)"
)
_RE_VARS = re.compile(r"vars: total=(\d+) decision=(\d+) defined=(\d+)")
_RE_SEARCH_VARS = re.compile(r"search vars: decision=(\d+) defined=(\d+)")
_RE_ALLDIFF = re.compile(
    r"all_different: vars=(\d+) \(unfixed=(\d+)\) vals=(\d+) ratio=([\d.]+)"
)
_RE_RESTART = re.compile(
    r"restart #(\d+) cl=(\d+) outer=(\d+) fails=(\d+)"
    r" max_depth=(\d+) nogoods=(\d+) prune=(\d+)"
)
_RE_CYCLE_END = re.compile(
    r"cycle end: prune_delta=(\d+) depth_grew=(true|false|0|1)"
    r" new_outer=(\d+)"
)
_RE_NEW_BEST = re.compile(r"new best objective: (-?\d+)")
_RE_OPTIMAL = re.compile(r"optimal \((.+)\)")
_RE_PROBE = re.compile(r"improvement probe: obj=\[(-?\d+), (-?\d+)\]")
_RE_BUMP = re.compile(r"bump_activity (enabled|disabled)")


def parse_verbose(stderr_text: str) -> dict[str, Any]:
    """Parse ``% [verbose] ...`` lines into structured data.

    Returns a dict with keys:

    - ``presolve``: dict with constraint/variable counts
    - ``vars``: total / decision / defined counts
    - ``search_vars``: decision / defined after presolve
    - ``all_different``: list of all_different statistics
    - ``restarts``: list of restart records
    - ``cycles``: list of cycle-end records
    - ``objectives``: list of improving objective values
    - ``outcome``: ``"feasible"`` / ``"optimal"`` / ``"timeout"`` / ``"infeasible"``
    - ``optimal_reason``: reason string if optimal
    """
    result: dict[str, Any] = {
        "presolve": {},
        "vars": {},
        "search_vars": {},
        "all_different": [],
        "restarts": [],
        "cycles": [],
        "objectives": [],
        "outcome": "unknown",
        "optimal_reason": None,
        "bump_activity": None,
    }

    for line in stderr_text.splitlines():
        line = line.strip()
        if "% [verbose]" not in line:
            continue
        msg = line.split("% [verbose]", 1)[1].strip()

        if m := _RE_PRESOLVE_START.search(msg):
            result["presolve"] = {
                "constraints": int(m.group(1)),
                "variables": int(m.group(2)),
                "avg_constraints_per_var": float(m.group(3)),
                "max_constraints_per_var": int(m.group(4)),
            }
        elif msg == "presolve failed":
            result["outcome"] = "infeasible"
            result["presolve"]["failed"] = True
        elif msg == "presolve done":
            result["presolve"]["failed"] = False
        elif m := _RE_VARS.search(msg):
            result["vars"] = {
                "total": int(m.group(1)),
                "decision": int(m.group(2)),
                "defined": int(m.group(3)),
            }
        elif m := _RE_SEARCH_VARS.search(msg):
            result["search_vars"] = {
                "decision": int(m.group(1)),
                "defined": int(m.group(2)),
            }
        elif m := _RE_ALLDIFF.search(msg):
            result["all_different"].append({
                "vars": int(m.group(1)),
                "unfixed": int(m.group(2)),
                "vals": int(m.group(3)),
                "ratio": float(m.group(4)),
            })
        elif m := _RE_RESTART.search(msg):
            result["restarts"].append({
                "number": int(m.group(1)),
                "conflict_limit": int(m.group(2)),
                "outer": int(m.group(3)),
                "fails": int(m.group(4)),
                "max_depth": int(m.group(5)),
                "nogoods": int(m.group(6)),
                "prune": int(m.group(7)),
            })
        elif m := _RE_CYCLE_END.search(msg):
            result["cycles"].append({
                "prune_delta": int(m.group(1)),
                "depth_grew": m.group(2) in ("true", "1"),
                "new_outer": int(m.group(3)),
            })
        elif m := _RE_NEW_BEST.search(msg):
            result["objectives"].append(int(m.group(1)))
        elif m := _RE_OPTIMAL.search(msg):
            result["outcome"] = "optimal"
            result["optimal_reason"] = m.group(1)
        elif "search stopped (timeout)" in msg:
            result["outcome"] = "timeout"
        elif m := _RE_BUMP.search(msg):
            result["bump_activity"] = m.group(1) == "enabled"

    # infer feasible if we never saw explicit outcome but got objectives
    if result["outcome"] == "unknown" and result["objectives"]:
        result["outcome"] = "feasible"

    return result


def diagnose(verbose_data: dict[str, Any]) -> list[str]:
    """Produce human-readable diagnostic hints from parsed verbose data.

    Returns a list of observation / suggestion strings.
    """
    hints: list[str] = []

    pre = verbose_data.get("presolve", {})
    if pre.get("failed"):
        hints.append(
            "Presolve detected infeasibility: the constraints are "
            "contradictory before search even begins.  Check for "
            "over-constrained variables or conflicting bounds."
        )
        return hints

    restarts = verbose_data.get("restarts", [])
    if len(restarts) > 50:
        last = restarts[-1]
        hints.append(
            f"High restart count ({len(restarts)}).  "
            f"Total fails={last['fails']}, nogoods={last['nogoods']}.  "
            "The problem may be very hard or constraints may be too weak "
            "for effective propagation."
        )

    if restarts:
        last = restarts[-1]
        if last["nogoods"] > 0 and last["prune"] == 0:
            hints.append(
                "Nogoods are being learned but never pruning.  "
                "This suggests the learned clauses are too specific "
                "to help.  Consider adding stronger global constraints."
            )
        if last["prune"] > 0:
            ratio = last["prune"] / max(1, last["nogoods"])
            if ratio > 0.5:
                hints.append(
                    f"Good nogood effectiveness: {last['prune']} prunes "
                    f"from {last['nogoods']} nogoods ({ratio:.0%})."
                )

    for ad in verbose_data.get("all_different", []):
        if ad["ratio"] > 0.95:
            hints.append(
                f"all_different constraint is tight: "
                f"{ad['unfixed']} unfixed vars for {ad['vals']} values "
                f"(ratio={ad['ratio']:.2f}).  This is nearly a "
                f"permutation — good for propagation."
            )
        elif ad["ratio"] < 0.3:
            hints.append(
                f"all_different constraint is loose: "
                f"{ad['unfixed']} unfixed vars for {ad['vals']} values "
                f"(ratio={ad['ratio']:.2f}).  Propagation will be weak; "
                f"consider tightening variable domains."
            )

    if verbose_data.get("outcome") == "timeout":
        hints.append(
            "Search timed out.  Consider: (1) adding redundant "
            "constraints to improve propagation, (2) increasing the "
            "timeout, or (3) using a different variable/value ordering."
        )

    if verbose_data.get("bump_activity") is False:
        hints.append(
            "Activity-based variable selection was disabled "
            "(problem structure too dense).  Search uses input order."
        )

    if not hints:
        hints.append("No issues detected.  Search appears healthy.")

    return hints


# ---------------------------------------------------------------------------
# Solving with timeout & verbose capture
# ---------------------------------------------------------------------------


def solve_with_timeout(
    model: CpModel,
    timeout_sec: float,
    solver: CpSolver | None = None,
    verbose: bool = False,
) -> dict[str, Any]:
    """Solve *model* with a timeout, optionally capturing verbose output.

    Returns a dict with keys:

    - ``status``: ``"FEASIBLE"`` / ``"OPTIMAL"`` / ``"INFEASIBLE"`` / ``"TIMEOUT"``
    - ``values``: solution dict or ``None``
    - ``stats``: solver statistics dict
    - ``verbose_text``: captured verbose stderr (if *verbose* is True)
    - ``diagnostics``: list of diagnostic hints (if *verbose* is True)
    """
    if solver is None:
        solver = CpSolver()
    if verbose:
        solver.set_verbose(True)

    result: dict[str, Any] = {
        "status": "INFEASIBLE",
        "values": None,
        "stats": {},
        "verbose_text": None,
        "diagnostics": None,
    }

    # Track whether the timer fired
    _timed_out = False

    def _on_timeout() -> None:
        nonlocal _timed_out
        _timed_out = True
        solver.stop()

    # Set up timeout via threading
    solver.reset_stop()
    timer = threading.Timer(timeout_sec, _on_timeout)

    # Capture stderr for verbose output.
    # We drain the pipe in a background thread to avoid deadlock when
    # the C++ solver writes more than the OS pipe buffer (~64 KB).
    old_fd: int | None = None
    r_fd: int | None = None
    w_fd: int | None = None
    drain_thread: threading.Thread | None = None
    stderr_chunks: list[bytes] = []

    def _drain_pipe() -> None:
        """Read from r_fd until EOF, appending to stderr_chunks."""
        assert r_fd is not None
        while True:
            chunk = os.read(r_fd, 65536)
            if not chunk:
                break
            stderr_chunks.append(chunk)

    if verbose:
        old_fd = os.dup(2)
        r_fd, w_fd = os.pipe()
        os.dup2(w_fd, 2)
        drain_thread = threading.Thread(target=_drain_pipe, daemon=True)
        drain_thread.start()

    try:
        timer.start()
        status = solver.solve(model)

        if status == SolveStatus.FEASIBLE:
            result["status"] = "FEASIBLE"
        elif status == SolveStatus.OPTIMAL:
            result["status"] = "OPTIMAL"
        else:
            result["status"] = "INFEASIBLE"
    except Exception as e:
        result["status"] = "ERROR"
        result["error"] = str(e)
    finally:
        timer.cancel()

        if verbose and old_fd is not None:
            # Restore stderr and close write end so the drain thread sees EOF
            os.dup2(old_fd, 2)
            os.close(old_fd)
            if w_fd is not None:
                os.close(w_fd)
            if drain_thread is not None:
                drain_thread.join(timeout=5.0)
            if r_fd is not None:
                os.close(r_fd)

            verbose_text = b"".join(stderr_chunks).decode("utf-8", errors="replace")
            result["verbose_text"] = verbose_text
            parsed = parse_verbose(verbose_text)
            result["diagnostics"] = diagnose(parsed)

            # Override status if verbose says timeout
            if parsed["outcome"] == "timeout":
                result["status"] = "TIMEOUT"

    # Detect timeout even when verbose is disabled
    if _timed_out and result["status"] in ("INFEASIBLE", "OPTIMAL"):
        result["status"] = "TIMEOUT"

    # Extract solution values
    if result["status"] in ("FEASIBLE", "OPTIMAL", "TIMEOUT"):
        values = {}
        for var in model._vars:
            try:
                values[var.name] = solver.value(var)
            except (RuntimeError, KeyError):
                pass
        if values:
            result["values"] = values

    # Extract stats
    try:
        st = solver.stats
        result["stats"] = {
            "fail_count": st.fail_count,
            "restart_count": st.restart_count,
            "nogood_count": st.nogood_count,
        }
    except Exception:
        pass

    return result
