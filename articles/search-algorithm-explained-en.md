# Inside sabori_csp's search: what's actually different from CDCL / LCG / MiniZinc solvers

> Prefer a quick read? Start with the [short version](search-algorithm-en-short.md). Original Japanese: [search-algorithm-explained.md](search-algorithm-explained.md).

## TL;DR

sabori_csp is a FlatZinc-compatible constraint solver written in C++. Its skeleton is completely standard:

> backtracking search + constraint propagation + restarts + activity-based variable selection + NoGood learning

There is no newly-invented "search algorithm" here. What makes it interesting is a thin self-tuning layer on top of that skeleton — and, more importantly, the fact that **every one of those additions was A/B-measured rather than asserted.** This article reports what worked and what didn't.

If I had to compress the whole article into one sentence answering its title:

> **LCG stops wasted search with *logic* — sound learned clauses prune deductively. sabori_csp stops it with *tendency* — the same conflict information is fed mainly into activity, steering variable selection away from bad regions.**

That claim isn't rhetorical; it falls out of an ablation that splits the NoGood's contribution into "pruning" and "activity bump" (Section 3). As a side effect, this write-up doubles as a small check of whether branching insights established for SAT/CDCL (activity implicitly capturing structure, etc.) reproduce in a CP solver.

On top of the standard skeleton, the four **adaptation layers** the solver adds are below (the strongest foundation is the primary criterion described later; these four sit on top of it). The headline result: **the thing that was effective was the *foundation*, not the clever refinements layered on top.** Variable selection is driven by two things sharing the labor: the primary criterion `temporal_activity` (Section 1, a Last-Conflict-style mechanism) picks the restart point after a backtrack, and **activity drives the bulk of the descent**. Removing temporal costs the most at the margin (net +25), but activity's true weight is masked by it: remove temporal and activity's ablation jumps to **+81** — it's the descent workhorse. The weak decision-trail NoGood (Section 3) feeds that activity. Of the four "smart adaptations" below, only the first (mix_p, which tunes the activity blend) clearly paid off; the rest didn't.

1. **Variable-selection mix ratio, tuned per-restart by a bandit** (→ measured robustness = works)
2. **A Bloom-fingerprint NoGood-overlap tiebreak for variable selection** (→ 93% no-op in A/B = doesn't help)
3. **explanation-free per-constraint conflict blame** (→ no gain over a generic version = doesn't help)
4. **a pseudo-gradient graft onto branch-and-bound** (→ negative on average, but problem-dependent = portfolio-only)

Setup for all measurements: same binary, toggled via environment variables (each defaults to current behavior), MiniZinc Challenge 2023+2024, 38 problems, 30s each, **5 seeds**, judged by objective value (not wall-clock time, which flips marginal ties). Each data point is one (problem × seed) pair. Scripts under `benchmarks/minizinc_challenge/`.

---

## 0. Background: the standard parts

For context, here are the components modern solvers share:

| Layer | SAT (CDCL) | CP / LCG | Examples |
|---|---|---|---|
| Variable selection | VSIDS (activity) | MRV / dom-wdeg / IBS / CHB | MiniSat, Chuffed, Choco |
| Value selection | phase saving | min/max/median, solution-guided | Glucose, OR-Tools |
| Conflict learning | 1-UIP clause learning | LCG (propagation → clauses) | Chuffed |
| Restarts | Luby / geometric / LBD-dynamic | same | Glucose |
| Fast propagation | 2-watched literal | domain propagation + watches | various |

sabori_csp implements the obvious ones straight: 2-watched-literal NoGood propagation, VSIDS-style activity (with decay/rescale), MRV (smallest domain first), restarts, branch-and-bound. So far, textbook. One twist matters, though: **the primary variable-selection criterion is not VSIDS or MRV but a different signal (`temporal_activity`), and that is what actually moves the search most** — it pays off later, so Section 1 covers it in detail. The rest of this article walks each axis and contrasts "standard" with "what sabori does."

---

## 1. Variable selection: learning the mix ratio instead of fixing it

### Prior art

Variable selection is the single biggest lever on CP/SAT performance, and many schemes exist: VSIDS (bump variables involved in conflicts, decay over time), MRV / fail-first (pick the smallest domain), dom/wdeg (domain size over constraint failure weight), IBS / CHB (impact- or reward-based), and **Last Conflict** (Lecoutre, Saïs, Tabary, Vidal 2009 — after a conflict, keep selecting the involved variable(s) until resolved; a cheap, strong conflict-directed ordering). In practice the tuning question is "how do you mix MRV and activity," and most solvers fix one or hard-code a static rule (e.g. MRV primary, activity as tiebreak).

### What sabori does, part 1: the primary criterion is conflict-directed `temporal_activity` (Last Conflict family)

Honestly, sabori's variable selection isn't primarily "activity vs MRV." **The top-level criterion is a separate signal, `temporal_activity`** (`src/core/variable_selector.cpp`): a per-variable counter that goes **+1 on conflict, −1 on a successful assignment**. The selection hierarchy is:

```
1. highest temporal_activity (a variable involved in a recent, unresolved conflict)   ← primary
2. (if tied) mix_p: activity-first or MRV-first                                        ← tiebreak
3. (if still tied) NoGood-Bloom overlap (Section 2)                                    ← deeper
```

So it prioritizes "keep digging at whatever is currently in conflict until it's resolved," and activity/MRV only decide among the variables that *aren't* in conflict (`temporal_activity = 0`). This is **inspired by Last Conflict** (Lecoutre et al. 2009): where classic LC forces the single last variable, this extends the idea to a counter that decays on success and spans multiple variables.

#### Measured: the single largest contributor

To see how much this primary criterion is worth, I toggled it on/off (env `SABORI_TEMPORAL`; off keeps the counter at 0 for all variables, falling through to mix_p/MRV). 38 problems × 5 seeds, objective value:

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| temporal_on vs temporal_off | 55 | 30 | 105 | **+25** |

net +25, **positive across all 5 seeds** (+5/+5/+4/+2/+9). That's the *marginal* cost of removing this primary criterion from the full system, and in that sense it's the largest single lever — a strong conflict-directed override, as the CP literature would predict.

But **the primary criterion being the biggest lever does *not* make activity a sideshow** — worth emphasizing. `temporal_activity` mostly decides the *first* pick right after a backtrack (the last-conflict variable); as search then descends normally, `temporal_activity` is ~0 for nearly all variables, and **the bulk of those descent selections are driven by activity (and MRV)** (instrumentation confirms: hot unassigned variables are only a few percent of selections on most problems). So **temporal picks the restart point and activity drives the descent** — they divide labor by search phase. Crucially, activity's effect is *masked* by temporal: removing the whole activity-bump machinery costs only +18 with temporal on, but **+81 with temporal off** (when activity drives every selection — Section 3). Activity is the descent workhorse, not a tiebreak; the mix_p below tunes *its* blend.

### What sabori does, part 2: a bandit over the tiebreak mix ratio

Only when the primary criterion (`temporal_activity`) ties does "activity-first vs MRV-first" come into play. sabori represents that **tiebreak mix ratio** as a 5-point grid (0.0 / 0.25 / 0.5 / 0.75 / 1.0) and **re-samples `mix_p` every restart via reinforcement learning** (`include/sabori_csp/mode_reward_policy.hpp`). It's a 5-arm multi-armed bandit: each arm holds an EMA reward; a recent improvement gives a strong reward, otherwise a weak "reach-depth" signal; the next ratio is drawn proportional to reward, with a floor for exploration and a 0.1× spillover to neighbors for smoothing. (VSIDS's own activity decay is itself exactly an EMA over the conflict signal — Liang & Ganesh et al. 2015, [arXiv:1506.08905](https://arxiv.org/abs/1506.08905), write normalized VSIDS as `s_n = (1−f)·δ_n + f·s_{n−1}`; here the same EMA idea is lifted one level up, from variable activity to heuristic reward.)

Where it sits: VSIDS and dom/wdeg adapt a *variable score*; sabori adapts the *heuristic itself* — one level more meta. To be upfront about prior art: **"use a multi-armed bandit to pick variable-selection heuristics online during CSP solving" is not new.** Xia & Yap, "Learning Robust Search Strategies Using a Bandit-Based Approach" ([arXiv:1805.03876](https://arxiv.org/abs/1805.03876), 2018), put a MAB over ddeg/dom, wdeg/dom, impact and activity, and reach **the same "robustness (avoid the worst fixed choice)" conclusion this article does.** That core idea is theirs.

sabori differs only in the details: (a) the arms are not distinct heuristics but a 5-point discretization of the *mix ratio* between two (MRV vs activity) — a continuous axis; (b) the bandit uses an **EMA reward with neighbor smoothing and an exploration floor**, not Thompson Sampling / UCB1; (c) it re-samples **per restart**, not per search node. So the framework is Xia & Yap's; the knobs (continuous mix, EMA, restart granularity) are where this differs.

#### Measured: adaptive buys robustness, not raw speed

Fixing vs adapting `mix_p` (env `SABORI_FIX_MIXP`), judged by objective value:

| Comparison | adaptive win | fixed win | tie |
|---|---|---|---|
| adaptive vs **always-MRV** (p=0) | **10** | 3 | 25 |
| adaptive vs **always-activity** (p=1) | 8 | 7 | 23 |
| adaptive vs **per-problem best-of-fixed** (oracle) | 8 | 8 | 22 |

The value isn't "beats every fixed heuristic" — it's **robustly avoiding the worst fixed choice per problem, and tracking best-of-fixed**. Read it as insurance for when you can't pick the right heuristic up front, not as a speedup.

---

## 2. NoGood-Bloom overlap: a tiebreak by "entanglement" with learned constraints

### What sabori does

sabori approximates, in constant time, "how much does this unassigned variable appear in the learned NoGoods that the variables I've chosen on this path appear in" (`src/core/variable_selector.cpp`). Each NoGood gets a `Bloom512` fingerprint; each variable carries the OR of the fingerprints of NoGoods it appears in (`var_ng_bloom`); as the path descends, the chosen variables' fingerprints accumulate into `ng_usage_bloom_`. At the *bottom* of the selection hierarchy — when the primary `temporal_activity` (Section 1), then activity/MRV, all tie — it prefers the candidate whose fingerprint overlaps most (AND + popcount). It's an idea I liked: context-dependent, approximate, constant-time. Note it's the deepest *tiebreak*, not the primary criterion — which is exactly why it rarely gets a turn (93% no-op, below).

#### Measured: it barely ever fires, and doesn't help

Toggling the tiebreak on/off (env `SABORI_BLOOM`; off means the path fingerprint is never accumulated):

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| bloom_on vs bloom_off | 5 | 9 | **176** | **−4** |

**176 of 190 are ties (93%).** The tiebreak changes the outcome in only 14 cases, and in those, off edges out on (9–5) — well within noise. So the tiebreak provides no measurable gain and is a no-op 93% of the time. Adding a per-candidate 512-bit AND + popcount to the variable-selection hot path doesn't earn its keep *as a tiebreak*. Soft verdict: keep the fingerprint infrastructure (`var_ng_bloom`), find another use for it, or remove.

Why doesn't it help? Because **the NoGood's main channel into variable selection is activity, not this tiebreak.** NoGoods bump the activity of involved variables both at learn time (`learn_from_conflict`) and when a learned NoGood fires and prunes (`propagate_*`). So "which learned constraints is the current region tied to" already reaches variable selection through the thick pipe of activity. The Bloom tiebreak tries to deliver the same information through a thinner separate pipe — and activity decides first, so it rarely gets a turn. This is the flip side of the community-analysis result (see the column): activity implicitly encodes structural centrality / which cluster a variable is near (Liang & Ganesh et al. 2015, [arXiv:1506.08905](https://arxiv.org/abs/1506.08905)), so an explicit structural-proximity tiebreak is redundant. Section 3 shows the NoGood itself *is* effective; this is its weak afterthought.

---

## 3. NoGood learning: not CDCL's 1-UIP, but the conjunction of the decision trail

### Prior art

- **CDCL**: walk the implication graph back to a 1-UIP cut, learn a minimal-ish strong clause, drive non-chronological backjumping.
- **LCG (Chuffed)**: teach every propagator to emit a sound *explanation* clause for each value removed, feed those into a real CDCL engine. Best of CP and SAT.
- **Generalized NoGoods from restarts** (Lecoutre et al., 2007): record a NoGood from the current decision sequence at restart. Weaker than 1-UIP, easy to implement.

### What sabori does: decision-trail NoGood

No implication-graph analysis, no LCG. On conflict, the current `decision_trail` (the branching literals so far) becomes a single NoGood (`src/core/nogood_manager.cpp`). Literals are `Eq` / `Leq` / `Geq`; propagation is 2-watched-literal, same as SAT; unit NoGoods apply directly to the domain.

This is clearly *weaker* learning than 1-UIP: it doesn't trace implications, the NoGood is made only of decision literals, and minimality isn't guaranteed. In exchange: no per-propagator explanation to write (none of LCG's heavy prerequisite), and a simple mechanism that applies uniformly to every constraint. "Less clever than LCG, but uniformly applicable lightweight learning" is the accurate framing. Why not 1-UIP / LCG? The trade-off between implementation cost and generality.

#### Measured: weak, but effective

Despite "weaker than LCG," the weak NoGood does help. Toggling learning + propagation entirely (env `SABORI_NOGOOD=0`):

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| ng_on vs ng_off | 54 | 36 | 100 | **+17** |

net +17 is modest, but **positive across all 5 seeds** (+4/+3/+2/+2/+7, no sign flip). Together with mix_p, this is one of the few consistently-positive results — in contrast to Bloom (Section 2), structural blame (Section 4), and gradient (Section 7), which all flipped sign across seeds.

#### Splitting the +17: pruning vs activity

`SABORI_NOGOOD=0` turns off the *whole* mechanism — both the clause pruning and the activity bumps NoGoods produce. To separate them, a third mode (`SABORI_NG_NOBUMP=1`) keeps learning + propagation (pruning) but drops only the NoGood-driven activity bumps:

| Comparison | what it measures | net |
|---|---|---|
| ng_full vs ng_prune | the activity-bump contribution | **+18** (seeds +9/+2/+2/0/+5, consistent) |
| ng_prune vs ng_off | pure pruning (learning + propagation only) | **+9** (seeds 0/+7/−1/0/+3, noisy) |
| ng_full vs ng_off | total (checksum) | +17 (≈ the +18 above) |

The reading is clear: **the NoGood's value flows mainly through activity.** Removing the activity bump costs +18 (and never flips sign), while pure pruning contributes a smaller, noisier +9. So "the weak learning works" is, in substance, more about *the learned clause reinforcing activity and steering variable selection* than about the clause pruning itself.

#### Going deeper: the activity supply is a redundant ensemble

So *which* of the activity bumps does the work? There are actually several conflict-driven bump paths: the NoGood **learn-time** bump (in `learn_from_conflict`, but at a tiny `activity_inc × 0.01` scale), the NoGood **propagation-time** bump (in `propagate_*`, full `activity_inc / n`), and the **decision-variable** bump (`handle_failure`, full `activity_inc`, always-on — the largest single bump). I toggled each one alone and in combination (`bench_ng_compensation.py`, same 38 problems × 5 seeds):

| Removed | net (how much worse than full) |
|---|---|
| learn bump only (0.01) | +7 |
| propagation bump only (full/n) | +2 |
| decision-variable bump only (full) | +8 |
| all three together | +14 |
| + the constraint bump too (= all conflict-driven activity bumps off) | **+21 (positive across all seeds)** |

Every prediction was wrong. By magnitude the smallest path — the 0.01 learn bump — beats the full-scale propagation bump on its own (+7 vs +2), and even the largest (decision-variable) is only +8 alone. **No single source is load-bearing; but the cost accumulates monotonically as you remove more, and removing all of them is the worst (+21, all seeds positive).** That's the signature of a **redundant ensemble**:

- **Activity-driven tendency control genuinely works** — turning off *all* conflict-driven bumps costs +21, consistently across seeds. The "tendency" thesis is reinforced, not weakened.
- **But the supply paths substitute for each other** — no single channel can be credited. Remove one and the others compensate. It's not "which channel" but "is there *enough* activity signal."
- **Per-event magnitude doesn't predict impact.** The 0.01 learn bump matters because it fires on every conflict across all decision-trail variables — cumulative effect = magnitude × frequency × breadth.

> Note: the earlier `ng_full vs ng_prune = +18` and the "+14 for all three" here don't match because 30s wall-clock judging wobbles run-to-run. The robust facts are the *monotonicity within one run* and "all-off = +21, every seed." Also, by `fails` the all-off config looks much *faster* — but by objective value it's the worst: it just fails less without searching productively. Judging by objective rather than node count was essential; node count would have given the opposite conclusion.

#### Important caveat: this +21 is the marginal effect, *masked* by temporal

That "+21 for all-off" is measured **with temporal on**. As Section 1 noted, temporal decides the first pick after a backtrack and activity drives the descent — so "how much does removing activity hurt" depends heavily on whether temporal is masking it. Measured both ways in one run (`bench_temporal_mask.py`): removing the whole activity-bump machinery costs **+18 with temporal on, but +81 with temporal off** (full beats no-activity 94–13, every seed). So **activity's true contribution is ~+81; temporal (and the MRV fallback) mask most of it, which is why the marginal ablation in the full system compresses to +21.** "Redundant ensemble" applies only to the activity-bump *sources* being mutually substitutable — **activity-based selection itself is the descent workhorse, not a sideshow.** It's the same masking the rest of the article keeps running into — a strong primary criterion hides the value of the layer beneath it — just one level larger.

This is the flip side of Section 2: because the NoGood reaches variable selection mainly via activity, the Bloom tiebreak — trying to deliver the same signal through a thinner pipe — never gets a turn. And by the same logic, refinements layered on the activity/value-selection that activity already dominates (Section 4, Section 7) tend to be hard to improve. **What's effective is the foundation that grows activity (NoGood, mix_p); cleverness piled on top of live activity tends not to pay.** That is the overall picture this article's measurements draw.

---

## 4. How does an explanation-free solver find the "real culprit" of a conflict? — per-constraint structural blame

This is the chapter where I describe a design I built to raise activity quality, then report — honestly — that A/B measurement showed no benefit. Appealing idea, unrewarded for now.

### Prior art: who do you blame on a conflict?

- **VSIDS**: bump variables in the 1-UIP learned clause; the implication-graph analysis decides who.
- **ABS** (Activity-Based Search): bump variables whose domain shrank.
- **dom/wdeg** (Boussemart et al., 2004): +1 a *per-constraint scalar* weight; uniform over the scope, coarse.
- **LCG / explanation**: each propagator emits a *sound* explanation clause and bumps the variables in it precisely. Highest precision, but the explanation must be sound (it's reused for learning), so it's heavy to implement.

### What sabori does: point at the culprit from each constraint's own structure

`Constraint::bump_activity` is `virtual`, and each global constraint overrides it to estimate the conflict's culprits at variable granularity from state it already maintains:

- **Base** (`src/core/constraint.cpp`): blame only variables whose bounds actually moved since presolve, weighted `activity_inc / (#moved) / domain_size`. Never-moved variables are presumed innocent; `1/domain_size` is a fail-first flavor.
- **IntLinEq**: look at the coefficients and which bound (lower/upper) was violated, distribute to the contributors.
- **AllDifferent**: when a fixed value `val` causes the wipeout, distribute only among variables still able to take `val` (its conflict set).
- **Circuit**: the `occupier_` array finds the colliding partner in O(1); split blame 50/50 between trigger and partner.

The framing I liked: a **"poor man's explanation"** — explanation-quality, variable-level blame, but heuristic-only, so it's allowed to be cheap and unsound (it's not reused for learning).

| Method | Blame granularity | Soundness required? |
|---|---|---|
| dom/wdeg | per-constraint scalar (uniform over scope) | no |
| VSIDS / ABS | learned-clause vars / shrunk vars | (learning side handles it) |
| LCG explanation | per-variable, precise | **yes** (reused for learning) |
| sabori | **per-variable, constraint-specific** | **no** (heuristic-only) |

#### Measured: the structural specialization didn't help

The blame layer has three levels (env `SABORI_BUMP_MODE`): **none** (no constraint-side bump), **base** (the generic poor man's explanation above), **structural** (the per-constraint overrides; the default).

| Comparison | left win | right win | tie | net |
|---|---|---|---|---|
| structural vs base | 37 | 41 | 112 | **−4** |
| base vs none | 34 | 41 | 115 | **−7** |
| structural vs none | 38 | 45 | 107 | **−7** |

What's refuted and what isn't, precisely:

- **The structural specialization doesn't beat base** (37–41); net −4 is noise-level and the sign flips per seed (base+7 on one seed, structural+2 on others). The per-constraint structural blame shows no gain over the generic version.
- **And the generic (base) doesn't beat none either.** Note that *base is* the poor man's explanation. base vs none is net −7 toward none — and per-seed it's none in 3, base in 1, tie in 1, a *consistent* thin lean (unlike structural-vs-base, which truly sign-flipped). So "base is a reasonable baseline" doesn't hold: even the generic constraint-side blame is, if anything, slightly worse than skipping it. This fits Section 3 exactly — the constraint bump is just one more member of the redundant activity-supply ensemble. When the decision-variable and NoGood bumps are already present, it's surplus (so removing it doesn't hurt); only when everything else is off does it contribute (+7, in the all-off measurement). "Redundant when others are present, helpful when they're absent" — textbook compensation.

A single seed first had me convinced structural was actively *worse* (base 9–6); five seeds erased it — narrow-sample win/loss flips with the seed.

#### Verdict: the whole constraint-side blame is surplus; structural is the worst of it

The measurement says two things. First, structural doesn't beat generic (the per-constraint cleverness is wasted). Second, the generic doesn't beat none either — the constraint-side blame is surplus *as a mechanism*. So the original bet, "poor man's explanation is a reasonable baseline," isn't supported here: the constraint bump is one more member of the redundant activity-supply ensemble (Section 3), and with the decision-variable and NoGood bumps already in place, it adds nothing.

This doesn't mean localization is wrong in principle — the design split (cheap heuristic blame vs sound learning) is still sensible, and the all-off measurement shows the constraint bump *does* help once everything else is removed. What's refuted is the bet that piling blame (generic or structural) on top of the existing activity supply makes search faster. And the current default of `structural` (the weakest of the three) is worth revisiting (though this is a head-to-head result — like the §7 probe, it could differ vs CP-SAT; that's unchecked here. See §7's methodological note). The pretty story I started writing — "smart blame compensates for weak learning" — was denied by measurement, and that denial is the most valuable thing in this chapter.

---

## 5. Restarts: not Luby or geometric, but inner/outer adaptive

sabori has a doubled inner/outer structure (`include/sabori_csp/restart_controller.hpp`). The inner loop grows the conflict limit by ~1.01 per restart; the outer loop stretches or shrinks the outer bound at the end of each cycle based on whether NoGood pruning advanced *and* the search went deeper:

```cpp
void end_cycle(size_t prune_delta, bool depth_grew) {
    if (prune_delta > 0 && depth_grew)
        outer_ = std::max(outer_ * 0.99, outer_min_);  // tighten (diversify)
    else
        outer_ = std::min(outer_ * 1.2, outer_max_);    // widen (persist)
}
```

Motivation is close to Glucose's LBD-dynamic restarts (detect stagnation and react), but the signal is "NoGood pruning progress × depth growth" rather than learned-clause quality. This is flavoring; I didn't ablate it.

---

## 6. Value selection and branching: enumerate / bisect hybrid

- **Hybrid branching**: domains ≤ a threshold (default 8) enumerate values; larger ones bisect (`src/core/solver_frame.cpp`). Which side first (`right_first`) is decided by hints.
- **Solution-guided value order**: bring the value from the best assignment so far to the front (`order_values`) — the CP analogue of phase saving.
- Under optimization, the pseudo-gradient hint below stacks on top of this value order — but **only inside Section 7's improvement-probe sub-search, never in the main branch-and-bound** (details in Section 7).

---

## 7. Optimization: an improvement probe, with a pseudo-gradient inside it

### What sabori does

On top of branch-and-bound, every time sabori finds an improving solution it runs one lightweight sub-search — the **improvement probe**. That probe is the container of this chapter, and the pseudo-gradient hint is a value-ordering option used *inside* that container. They are not two independent mechanisms: the gradient is **nested inside the probe**. The code works that way too — the gradient hint is set only when a new best is found (`gradient_strategy_.compute`) and is always disabled right after the probe's sub-search returns (`disable_hint`); **the main branch-and-bound only ever runs with the hint disabled**.

- **Improvement probe** (`run_improvement_probe`, `probe_fail_limit_`): on each improving solution, run one fail-bounded sub-search aiming at a "~5% of the objective range" improvement from the best-objective side. A hit jumps the objective; a miss is cut off after few failures.
- **Pseudo-gradient hint** (`include/sabori_csp/gradient_strategy.hpp`): the value-ordering bias *for that probe sub-search*. From the difference between consecutive improving solutions it estimates each variable's improving direction (went up / down), picks one low-activity (= barely-conflicted = high-freedom) variable, and biases its value ordering toward the gradient direction inside the probe.

Note this is a **value-selection** heuristic — a different axis from the variable-selection / activity discussion in Sections 2 and 4.

#### Measured: the gradient inside the probe, also no gain on average

First the option inside the container. Toggling the gradient hint on/off (env `SABORI_GRADIENT`; off falls back to solution-guided value order in the probe sub-search). **The gradient only ever fires inside the probe**, so this measures the **marginal effect of the gradient option assuming the probe is on** (its default) — turn the probe off and the gradient does nothing. The gradient only fires on problems with an objective, so this is the 36 optimization problems × 5 seeds = 180 points:

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| grad_on vs grad_off | 12 | 25 | 143 | **−13** |

80% ties; among the decisive ones, off wins 25–12. On average the gradient hint provides no gain and leans slightly negative (the net wobbles run-to-run under wall-clock judging — another run gave −7 — but the direction is stably negative).

#### Pulling out the winners

But "negative on average" is half the story. I first hypothesized "tight problems (high constraint density) throw the gradient off" and split by constraint-lines / variable-lines — that didn't separate them (loose net −8 / tight net −5, both negative). Listing the actual winners and losers tells a different story:

| | problems (on/off/tie over 5 seeds) |
|---|---|
| **gradient helped** | valve-network (3/0/2), cable-tree-wiring (3/2/0), code-generator (2/0/3), harmony (1/0/4) |
| **gradient backfired** | hoist-benchmark (0/5/0), train-scheduling (0/3/2), unit-commitment (1/3/1), test-scheduling (0/2/3), yumi-static (0/2/3), aircraft-disassembly (1/2/2), roster-shifts-bool (0/2/3) |

**The backfiring side is heavily scheduling / timetabling** (hoist-benchmark loses all 5 seeds; train/test-scheduling, unit-commitment, roster, aircraft-disassembly). A small-sample observation-as-hypothesis: scheduling has start times tightly coupled by resource constraints, so "the direction that helped last time" gets betrayed by collisions next time. Design/assignment problems like valve-network and code-generator have more independent variables, and the direction bias carries cleanly. (Winners 4, losers 7, many ties; problem-type labels are manual — observation, not proof.)

> **Having problem-specific strengths and weaknesses isn't inherently a flaw.** For a single solver running it always-on it averages negative, so you'd drop it. But assume a parallel portfolio / ensemble: run gradient-on and gradient-off in separate threads and take whichever finishes first, and the disagreement ("on wins valve-network, off wins hoist-benchmark") becomes diversity = an asset. Same spirit as mix_p (Section 1) as "insurance against the worst fixed choice": the gradient loses on its own average but can be worth a slot in a portfolio.

#### Measured: the probe — where "head-to-head" and "vs CP-SAT" disagree

Now the container itself: toggle `run_improvement_probe` as a whole (env `SABORI_PROBE`, `benchmarks/minizinc_challenge/bench_probe.py`). Since the gradient is a value-ordering option *inside* the probe, turning the probe off removes the gradient's effect too — so this measures the **probe+gradient bundle** versus **neither**.

By the same **config-vs-config head-to-head (objective)** used throughout this article, the probe loses: 36 optimization problems × 5 seeds give net **−23** (probe_off wins 47–24, ahead in 4 of 5 seeds, probe_on never winning). On a different year set (2016+2025) it's net −13 — the direction ("off gives better objectives") reproduces.

But here, **the metric you pick flips the verdict.** The actual Challenge goal isn't "which of two configs has the better objective" — it's "**how many problems do you win against the field (CP-SAT)?**" Counting **Sabori vs CP-SAT** wins on the **same primary set 2023+2024** as the −23 head-to-head, with the probe on vs off:

| Config | Sabori wins | CP-SAT wins | ties |
|---|---|---|---|
| **probe on (default)** | **14** | 13 | 9 |
| **probe off** | 12 | 14 | 10 |

**The probe-on config wins more (14 vs 12) and loses one fewer to CP-SAT (13 vs 14)** — the config that loses head-to-head wins on *both* axes of the field metric. A different year set (2016+2025) points the same way (probe on 15 vs off 12; CP-SAT wins tied at 17). The reason: the probe **slightly worsens the objective on many problems** (hence the head-to-head loss) but **pushes a few across CP-SAT's threshold into wins** (hence the vs-field gain).

To be honest, the margin is thin: on 2023+2024 only a couple of 2023 problems moved (2024 ties at the aggregate), and the vs-CP-SAT verdict is single-seed, so it wobbles run-to-run. Still, **two independent year sets (2016+2025 and 2023+2024) both point to "probe on ≥ off,"** so I take the direction as solid.

> **Methodological note (applies to the whole article).** This article's ablations are mostly **config-vs-config head-to-head by objective** — "which is better on average." The Challenge goal is "beat the field," where a few threshold-crossing problems decide it. The two metrics are usually close but not identical. **The other "doesn't help / surplus" conclusions here (Bloom in §2, structural blame in §4, the gradient above) are all head-to-head too** — and the probe is the *only* one I checked against the field, where it flipped. The rest look consistent by informal observation, but I haven't verified them vs the field, so read those negative conclusions as "head-to-head, at least." On the head-to-head −23 alone the probe looks droppable; against CP-SAT it wins more, so **keeping it is the right call** — a concrete case where the metric you pick changes the verdict.

---

## 8. A presolve technique: one-hot channeling aggregation

A presolve pass detects groups of `int_eq_reif` (`b_i ⇔ (x == v_i)`) hanging off the same integer `x` and fuses them into a single `IntOneHotChannelConstraint` (`include/sabori_csp/one_hot_channel_aggregator.hpp`). FlatZinc tends to expand integers into one-hot boolean groups that arrive as many tiny reif constraints; merging them into one channel constraint lets a single propagation enforce the whole one-hot's consistency.

#### Measured: unlike the search-heuristic refinements, this works

Toggling aggregation on/off (env `SABORI_ONEHOT`). On problems where no aggregation fires, on/off are identical, so this is restricted to the **16 problems where aggregation actually happens** × 5 seeds = 80 points:

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| onehot_on vs onehot_off | 25 | 19 | 36 | **+6** |

net +6, on ahead (non-negative in 4 of 5 seeds). Not the consistent blowout of the NoGood (Section 3), but positive — and clearly on the *other* side from the search-heuristic refinements (Sections 2/4/7). And +6 *understates* it: `code-generator` runs at `fails=3218` with aggregation on vs `10867` off — search effort drops ~70%. The aggregate stays modest because at 30s judged by objective, "fewer fails but the same objective in time" registers as a tie (most of those 36 ties). The effect is "reach the same solution with less search," which a time-to-solution metric or a tighter budget would show more strongly. As a model-reduction presolve, it straightforwardly works — fitting the larger picture: **what's effective is the variable-selection foundation (the temporal_activity primary criterion, plus the NoGood and mix_p that feed activity) and the model transform (one-hot), not the clever refinements piled on top.**

---

## Column: community analysis is not "search speedup"

sabori has a `-c` community-analysis feature (`include/sabori_csp/community_analysis.hpp`): build a Variable Interaction Graph (VIG), detect communities by label propagation, compute modularity Q, and measure how locally search stays within communities. It looks appealing, but as the header states, it's **diagnostics only** — no benchmark showed a search speedup, because activity heuristics implicitly learn the same locality. The correct framing is "structure visualization / sticking diagnosis," and that negative result ("tried explicit community structure; activity already learns it implicitly") is itself the useful knowledge.

This isn't a new observation — it lines up with prior work. Liang & Ganesh et al., "Understanding VSIDS Branching Heuristics" ([arXiv:1506.08905](https://arxiv.org/abs/1506.08905), 2015), show that **VSIDS overwhelmingly picks the "bridge variables" connecting communities without any explicit community detection** (~80% of picked variables are bridges), and that activity ranks correlate strongly with temporal graph centrality (Spearman 0.79–0.82). Because activity already captures structural centrality, decomposing the VIG explicitly adds nothing — this column's result is a re-confirmation of that, not a discovery.

---

## Summary: standard skeleton + a thin adaptive layer, measured

| Axis | Prior art | sabori_csp |
|---|---|---|
| Variable selection (restart override) | Last Conflict / conflict-history | **`temporal_activity`** (LC family, conflict recency) — picks the post-backtrack pick; largest marginal ablation (net +25) |
| Variable selection (descent driver) | fixed/static mix of VSIDS / MRV / dom-wdeg | activity (drives most descent picks; masked by temporal to marginal +21, but +81 with temporal off = the real workhorse) + **bandit-learned mix** (mix_p) |
| Variable-selection context | folded into activity | **NoGood-Bloom overlap** tiebreak — 93% no-op; another use or remove |
| Conflict learning | 1-UIP / LCG | **decision-trail conjunction** — weak but A/B-positive across 5 seeds |
| Conflict blame (activity) | dom/wdeg / LCG explanation | **poor man's explanation**; in A/B neither structural nor generic beats *none* — a redundant member of the activity-supply ensemble, surplus as a mechanism |
| Propagation | 2-watched literal | same |
| Restarts | Luby / geometric / LBD | **inner/outer adaptive** (prune × depth) |
| Value selection | phase saving / solution-guided | solution-guided + **pseudo-gradient** (fires only inside the probe below; problem-dependent; portfolio-only) |
| Optimization | branch-and-bound / LNS | branch-and-bound + **improvement probe** (contains the gradient hint; loses head-to-head, net −23, but wins more vs CP-SAT = worth keeping) |
| Presolve | generic constraint fusion | **one-hot channel aggregation** — A/B +6, large search-effort drop |
| Structure analysis | (none) | community analysis (**diagnostics only**) |

No world-first algorithm appears here. The value is in measuring each deviation and reporting both the wins and the losses:

- **Effective (foundation + model transform):** variable selection is two labor-sharing axes — `temporal_activity` (Section 1, Last-Conflict-style) overrides the post-backtrack pick (largest marginal ablation, net +25, all seeds), and **activity drives the descent** (masked by temporal to a marginal +21, but +81 with temporal off = the real workhorse). The weak decision-trail NoGood (Section 3, +17) feeds that activity; one-hot aggregation (Section 8, +6) shrinks the model. The mix_p bandit (Section 1) tunes the activity blend and shows "avoid-the-worst-fixed" robustness.
- **No measurable gain (refinements on top):** Bloom tiebreak (Section 2, 93% no-op); constraint-side blame (Section 4 — not just the structural specialization, the generic version doesn't beat *none* either, so the whole mechanism is surplus). The interesting part is that the layers trying to add cleverness on top of the effective foundation (NoGood, activity) consistently go unrewarded — and the activity supply itself turns out to be a redundant ensemble (Section 3), so an extra blame channel is just surplus."
- **Problem-dependent (portfolio-only):** the pseudo-gradient hint (Section 7) is a value-ordering option that fires only inside the probe sub-search below. Given the probe, it loses on average but splits by problem (backfires on resource-coupled scheduling, helps on design/assignment); worth a portfolio slot, not an always-on default.
- **Metric-dependent:** the improvement probe (Section 7, a ~5%-improvement sub-search that contains the gradient hint above) loses the config-vs-config head-to-head by objective (net −23) but **wins more against CP-SAT** (14 vs 12 on the same primary set 2023+2024, with one fewer CP-SAT win; 15 vs 12 on 2016+2025). On the head-to-head alone the probe looks droppable, but it wins more vs CP-SAT, so keeping it is right. The ablation metric (head-to-head) and the Challenge goal (beat the field) mostly agree, but this is where they diverge.

### LCG stops wasted search with logic; sabori stops it with tendency

The opening question — "what's different from LCG" — now has a one-line answer earned by measurement:

> **LCG uses clauses learned from conflicts as sound logical constraints to *deductively* prune regions it provably never revisits. sabori_csp feeds the same conflict information mainly into activity and controls the *tendency* of variable selection so it steers away from bad regions.**

Same goal (cut wasted search), different principle: **logic (deductive pruning)** vs **tendency (heuristic control)**. And this isn't speculation — Section 3's split shows it directly: sabori's NoGood does prune deductively too (pure pruning +9), but what's actually working is mostly the part that fattens activity to shift the tendency (+18). **sabori has logic, but the main reason it cuts waste is on the tendency side.**

Seen this way, the results line up. The "tendency" that decides search has two pillars that **divide labor by search phase**: **`temporal_activity` (Last-Conflict family) overrides the restart point after each backtrack, and VSIDS-style activity (fed by NoGood) drives the bulk of the descent.** Both are heuristic "lean toward what recently conflicted," not logic. If these two pillars decide search, then **the foundations that feed them (temporal_activity, NoGood, mix_p) work, and the layers that try to micro-adjust the same tendency through a different route (Bloom tiebreak, structural blame) go unrewarded because the thick pillars decide first.** One-hot aggregation is the exception only because its route is different — it shrinks the model rather than the tendency, so it helps independently. The gradient hint (Section 7) isn't on this axis (variable-selection tendency) at all — it's a value-ordering bias inside the probe, and it fails for a different reason (direction estimation depends on problem structure) — a separate mechanism.

#### A side effect: replicating a SAT lesson in CP

This write-up also doubles as a small cross-domain check: do branching insights established for SAT/CDCL hold in a CP solver? Liang & Ganesh (2015), cited above, is a **SAT** result — "activity captures structural centrality (bridge variables) without explicit detection, so activity is a strong structural signal." This article's negative results (explicit community analysis adds nothing; the Bloom tiebreak is redundant) are **that SAT lesson re-confirmed in CP**. And the "tendency" framing itself is, at bottom, a measurement of **how far the SAT lesson "activity is the dominant structural signal" carries into CP**. No world-first — but cross-domain reproducibility is a different kind of value, and it's the one this article actually delivers.

The stance toward CDCL / LCG / MiniZinc-family solvers isn't "win with a clever trick" — it's "a lightweight solver that stops waste with tendency rather than logic, and measures what works and what doesn't, in the open."

---

### Reproducing the measurements

All toggles default to current behavior; benchmark scripts are self-contained.

| What | Env toggle | Bench script |
|---|---|---|
| mix_p bandit | `SABORI_FIX_MIXP` | `bench_mixp.py` |
| Bloom tiebreak | `SABORI_BLOOM` | `bench_bloom.py` |
| decision-trail NoGood | `SABORI_NOGOOD` / `SABORI_NG_NOBUMP` | `bench_nogood.py` / `bench_ng_split.py` |
| constraint blame | `SABORI_BUMP_MODE` / `SABORI_BUMP_STRUCT_ONLY` | `bench_bump.py` / `bench_bump_perconstraint.py` |
| pseudo-gradient | `SABORI_GRADIENT` | `bench_gradient.py` |
| one-hot aggregation | `SABORI_ONEHOT` | `bench_onehot.py` |
| multi-seed | `SABORI_SEED` | (all of the above) |

### References

- Moskewicz, Madigan, Zhao, Zhang, Malik, "Chaff: Engineering an Efficient SAT Solver", DAC, 2001. — origin of VSIDS (the branching heuristic referenced throughout).
- Boussemart, Hemery, Lecoutre, Saïs, "Boosting Systematic Search by Weighting Constraints", ECAI, 2004. — dom/wdeg (per-constraint weighting, Section 4).
- Lecoutre, Saïs, Tabary, Vidal, "Recording and Minimizing Nogoods from Restarts", JSAT, 2007. — recording NoGoods from the decision sequence at restart (Section 3).
- Lecoutre, Saïs, Tabary, Vidal, "Reasoning from last conflict(s) in constraint programming", Artificial Intelligence 173(18), 2009. — Last Conflict (prefer the conflict-involved variable until resolved); sabori's primary criterion `temporal_activity` is inspired by it, extended to a decaying multi-variable counter (Section 1).
- Liang, Ganesh, Zulkoski, Zaman, Czarnecki, "Understanding VSIDS Branching Heuristics in Conflict-Driven Clause-Learning SAT Solvers", [arXiv:1506.08905](https://arxiv.org/abs/1506.08905), 2015. — VSIDS decay as an EMA; activity implicitly captures bridge variables / graph centrality (Sections 1, 2, and the community column).
- Xia, Yap, "Learning Robust Search Strategies Using a Bandit-Based Approach", [arXiv:1805.03876](https://arxiv.org/abs/1805.03876), 2018. — prior work on online MAB selection of variable-ordering heuristics for robustness (Section 1).

### Source files referenced

- Search loop: `src/core/solver_search.cpp`, `src/core/solver_frame.cpp`, `include/sabori_csp/solver.hpp`
- Bandit mix: `include/sabori_csp/mode_reward_policy.hpp`
- Variable selection / Bloom tiebreak: `src/core/variable_selector.cpp`
- NoGood learning / propagation: `src/core/nogood_manager.cpp`
- Per-constraint activity blame: `include/sabori_csp/constraint.hpp`, `src/core/constraint.cpp` (base), `int_lin_eq.cpp` / `all_different.cpp` / `circuit.cpp` (structural)
- Restart control: `include/sabori_csp/restart_controller.hpp`
- Pseudo-gradient: `include/sabori_csp/gradient_strategy.hpp`
- One-hot aggregation: `include/sabori_csp/one_hot_channel_aggregator.hpp`
- Community analysis (diagnostics only): `include/sabori_csp/community_analysis.hpp`
