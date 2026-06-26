# Inside sabori_csp's search: what's actually different from CDCL / LCG / MiniZinc solvers

> Prefer a quick read? Start with the [short version](search-algorithm-en-short.md). Original Japanese: [search-algorithm-explained.md](search-algorithm-explained.md).

## TL;DR

sabori_csp is a FlatZinc-compatible constraint solver written in C++. Its skeleton is completely standard:

> backtracking search + constraint propagation + restarts + activity-based variable selection + NoGood learning

There is no newly-invented "search algorithm" here. What makes it interesting is a thin self-tuning layer on top of that skeleton — and, more importantly, the fact that **every one of those additions was A/B-measured rather than asserted.** This article reports what worked and what didn't, honestly.

If I had to compress the whole article into one sentence answering its title:

> **LCG stops wasted search with *logic* — sound learned clauses prune deductively. sabori_csp stops it with *tendency* — the same conflict information is fed mainly into activity, steering variable selection away from bad regions.**

That claim isn't rhetorical; it falls out of an ablation that splits the NoGood's contribution into "pruning" and "activity bump" (Section 3).

The four places the solver deviates from the standard recipe are below. The headline result: **the thing that was effective was the *foundation*, not the clever refinements layered on top.** The weak decision-trail NoGood learning (Section 3, part of the "standard" skeleton) was positive across all 5 seeds despite being weaker than LCG; of the four "smart adaptations" below, only the first (mix_p) clearly paid off.

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

sabori_csp implements the obvious ones straight: 2-watched-literal NoGood propagation, VSIDS-style activity (with decay/rescale), MRV (smallest domain first), restarts, branch-and-bound. So far, textbook. The rest of this article walks each axis and contrasts "standard" with "what sabori does."

---

## 1. Variable selection: learning the mix ratio instead of fixing it

### Prior art

Variable selection is the single biggest lever on CP/SAT performance, and many schemes exist: VSIDS (bump variables involved in conflicts, decay over time), MRV / fail-first (pick the smallest domain), dom/wdeg (domain size over constraint failure weight), IBS / CHB (impact- or reward-based). In practice the tuning question is "how do you mix MRV and activity," and most solvers fix one or hard-code a static rule (e.g. MRV primary, activity as tiebreak).

### What sabori does: a bandit over the mix ratio

sabori represents "activity-first vs MRV-first" as a 5-point grid (0.0 / 0.25 / 0.5 / 0.75 / 1.0) and **re-samples the mix ratio `mix_p` every restart via reinforcement learning** (`include/sabori_csp/mode_reward_policy.hpp`). It's a 5-arm multi-armed bandit: each arm holds an EMA reward; a recent improvement gives a strong reward, otherwise a weak "reach-depth" signal; the next ratio is drawn proportional to reward, with a floor for exploration and a 0.1× spillover to neighbors for smoothing.

Where it sits: VSIDS and dom/wdeg adapt a *variable score*; sabori adapts the *mix of heuristics themselves* — one level more meta. Bandit-based strategy selection exists in the literature (restart/heuristic portfolio selection), but is uncommon inside a lightweight CP solver.

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

sabori approximates, in constant time, "how much does this unassigned variable appear in the learned NoGoods that the variables I've chosen on this path appear in" (`src/core/variable_selector.cpp`). Each NoGood gets a `Bloom512` fingerprint; each variable carries the OR of the fingerprints of NoGoods it appears in (`var_ng_bloom`); as the path descends, the chosen variables' fingerprints accumulate into `ng_usage_bloom_`. When MRV and activity tie, it prefers the candidate whose fingerprint overlaps most (AND + popcount). It's an idea I liked: context-dependent, approximate, constant-time. Note it's only a *tiebreak*, not the primary criterion.

#### Measured: it barely ever fires, and doesn't help

Toggling the tiebreak on/off (env `SABORI_BLOOM`; off means the path fingerprint is never accumulated):

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| bloom_on vs bloom_off | 5 | 9 | **176** | **−4** |

**176 of 190 are ties (93%).** The tiebreak changes the outcome in only 14 cases, and in those, off edges out on (9–5) — well within noise. So the tiebreak provides no measurable gain and is a no-op 93% of the time. Adding a per-candidate 512-bit AND + popcount to the variable-selection hot path doesn't earn its keep *as a tiebreak*. Soft verdict: keep the fingerprint infrastructure (`var_ng_bloom`), find another use for it, or remove.

Why doesn't it help? Because **the NoGood's main channel into variable selection is activity, not this tiebreak.** NoGoods bump the activity of involved variables both at learn time (`nogood_manager.cpp:223`) and when a learned NoGood fires and prunes (`:74/100/120`). So "which learned constraints is the current region tied to" already reaches variable selection through the thick pipe of activity. The Bloom tiebreak tries to deliver the same information through a thinner separate pipe — and activity decides first, so it rarely gets a turn. Section 3 shows the NoGood itself *is* effective; this is its weak afterthought.

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
| ng_full vs ng_off | total (checksum) | +17 (matches the +18 above) |

The reading is blunt: **the NoGood's value flows mainly through activity.** Removing the activity bump costs +18 (and never flips sign), while pure pruning contributes a smaller, noisier +9. So "the weak learning works" is, in substance, more about *the learned clause fattening activity and steering variable selection* than about the clause pruning itself.

This is the flip side of Section 2: because the NoGood reaches variable selection mainly via activity, the Bloom tiebreak — trying to deliver the same signal through a thinner pipe — never gets a turn. And by the same logic, refinements layered on the activity/value-selection that activity already dominates (Section 4, Section 7) tend to be hard to improve. **What's effective is the foundation that grows activity (NoGood, mix_p); cleverness piled on top of live activity tends not to pay.** That's the biggest picture this article's measurements draw.

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

- **Refuted: the structural specialization.** structural doesn't beat base (37–41); net −4 is noise-level and the sign flips per seed (base+7 on one seed, structural+2 on others). The per-constraint structural blame layered on the generic poor man's explanation shows no gain over the generic version.
- **Not refuted: poor man's explanation itself.** Note that *base is* the poor man's explanation. base vs none is net −7 toward none, but that's inside the same noise band — not enough to call localization useless either. What we can say is narrow: "the structural elaboration doesn't beat generic," not "poor man's explanation is worthless."

A single seed first had me convinced structural was actively *worse* (base 9–6); five seeds erased it — narrow-sample win/loss flips with the seed.

#### Verdict: keep poor man's explanation, structural is future work

What's refuted is the *per-constraint structural* elaboration, not the idea of cheap unsound localization. The likely reason: each structural override was added at a different point in development, and they plausibly interfere with optimizations elsewhere (variable selection, restarts, value ordering), canceling out in aggregate. Kept on by default but reclassified as future work. The pretty story I started writing — "smart blame compensates for weak learning" — was denied by measurement, and that denial is the most valuable thing in this chapter.

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

- **Hybrid branching**: domains ≤ a threshold (default 8) enumerate values; larger ones bisect (`src/core/solver_frame.cpp`). Which side first (`right_first`) is decided by hints/gradient.
- **Solution-guided value order**: bring the value from the best assignment so far to the front (`order_values`) — the CP analogue of phase saving.
- Under optimization, the pseudo-gradient hint below stacks on top.

---

## 7. Optimization: grafting a pseudo-gradient onto branch-and-bound

### What sabori does

On top of branch-and-bound, sabori estimates a per-variable "improving direction" from the difference between consecutive improving solutions (`include/sabori_csp/gradient_strategy.hpp`): compare the last two improving solutions, set the gradient sign per variable, pick one low-activity variable as a "gradient hint," and bias its value ordering toward the gradient direction (also feeding a lightweight "improvement probe"). Note this is a **value-selection** heuristic — a different axis from the variable-selection / activity discussion in Sections 2 and 4.

#### Measured: also no gain on average

Toggling the gradient hint on/off (env `SABORI_GRADIENT`; off falls back to solution-guided value order). The gradient only fires on problems with an objective, so this is the 36 optimization problems × 5 seeds = 180 points:

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

---

## 8. A presolve technique: one-hot channeling aggregation

A presolve pass detects groups of `int_eq_reif` (`b_i ⇔ (x == v_i)`) hanging off the same integer `x` and fuses them into a single `IntOneHotChannelConstraint` (`include/sabori_csp/one_hot_channel_aggregator.hpp`). FlatZinc tends to expand integers into one-hot boolean groups that arrive as many tiny reif constraints; merging them into one channel constraint lets a single propagation enforce the whole one-hot's consistency.

#### Measured: unlike the search-heuristic refinements, this works

Toggling aggregation on/off (env `SABORI_ONEHOT`). On problems where no aggregation fires, on/off are identical, so this is restricted to the **16 problems where aggregation actually happens** × 5 seeds = 80 points:

| Comparison | on win | off win | tie | net |
|---|---|---|---|---|
| onehot_on vs onehot_off | 25 | 19 | 36 | **+6** |

net +6, on ahead (non-negative in 4 of 5 seeds). Not the consistent blowout of the NoGood (Section 3), but positive — and clearly on the *other* side from the search-heuristic refinements (Sections 2/4/7). And +6 *understates* it: `code-generator` runs at `fails=3218` with aggregation on vs `10867` off — search effort drops ~70%. The aggregate stays modest because at 30s judged by objective, "fewer fails but the same objective in time" registers as a tie (most of those 36 ties). The effect is "reach the same solution with less search," which a time-to-solution metric or a tighter budget would show more strongly. As a model-reduction presolve, it straightforwardly works — fitting the larger picture: **what's effective is the foundation (NoGood, mix_p) and the model transform (one-hot), not the clever refinements piled on activity/value-selection.**

---

## Column: community analysis is not "search speedup"

sabori has a `-c` community-analysis feature (`include/sabori_csp/community_analysis.hpp`): build a Variable Interaction Graph (VIG), detect communities by label propagation, compute modularity Q, and measure how locally search stays within communities. It looks appealing, but as the header states, it's **diagnostics only** — no benchmark showed a search speedup, because activity heuristics implicitly learn the same locality. The correct framing is "structure visualization / sticking diagnosis," and that negative result ("tried explicit community structure; activity already learns it implicitly") is itself the useful knowledge.

---

## Summary: standard skeleton + a thin adaptive layer, measured

| Axis | Prior art | sabori_csp |
|---|---|---|
| Variable selection | fixed/static mix of VSIDS / MRV / dom-wdeg | **bandit-learned mix** (mix_p) — works |
| Variable-selection context | folded into activity | **NoGood-Bloom overlap** tiebreak — 93% no-op; another use or remove |
| Conflict learning | 1-UIP / LCG | **decision-trail conjunction** — weak but A/B-positive across 5 seeds |
| Conflict blame (activity) | dom/wdeg / LCG explanation | **poor man's explanation**; per-constraint structural elaboration showed no gain → future work |
| Propagation | 2-watched literal | same |
| Restarts | Luby / geometric / LBD | **inner/outer adaptive** (prune × depth) |
| Value selection | phase saving / solution-guided | solution-guided + **pseudo-gradient** (problem-dependent; portfolio-only) |
| Optimization | branch-and-bound / LNS | branch-and-bound + gradient probe |
| Presolve | generic constraint fusion | **one-hot channel aggregation** — A/B +6, large search-effort drop |
| Structure analysis | (none) | community analysis (**diagnostics only**) |

No world-first algorithm appears here. The value is in measuring each deviation and reporting both the wins and the losses:

- **Effective (foundation + model transform):** the bandit mix (Section 1, robustness); decision-trail NoGood (Section 3, positive across all 5 seeds despite being weaker than LCG); one-hot aggregation (Section 8, net +6, large search-effort reduction).
- **No measurable gain (refinements on top):** Bloom tiebreak (Section 2, 93% no-op); per-constraint structural blame (Section 4, no gain over generic). The interesting part is that the layers trying to add cleverness on top of the effective foundation (NoGood, activity) consistently go unrewarded — consistent with "activity decides first, so the tiebreak / blame refinement never gets a turn."
- **Problem-dependent (portfolio-only):** the pseudo-gradient (Section 7) loses on average but splits by problem (backfires on resource-coupled scheduling, helps on design/assignment); worth a portfolio slot, not an always-on default.

### LCG stops wasted search with logic; sabori stops it with tendency

The opening question — "what's different from LCG" — now has a one-line answer earned by measurement:

> **LCG uses clauses learned from conflicts as sound logical constraints to *deductively* prune regions it provably never revisits. sabori_csp feeds the same conflict information mainly into activity and controls the *tendency* of variable selection so it steers away from bad regions.**

Same goal (cut wasted search), different principle: **logic (deductive pruning)** vs **tendency (heuristic control)**. And this isn't speculation — Section 3's split shows it directly: sabori's NoGood does prune deductively too (pure pruning +9), but what's actually working is mostly the part that fattens activity to shift the tendency (+18). **sabori has logic, but the main reason it cuts waste is on the tendency side.**

Seen this way, the results line up. If activity — the "tendency pipe" — is what decides search, then **the foundations that feed it (NoGood, mix_p) work, and the layers that try to micro-adjust the same tendency through a different route (Bloom tiebreak, structural blame, gradient) go unrewarded because the thick pipe decides first.** One-hot aggregation is the exception only because its route is different — it shrinks the model rather than the tendency, so it helps independently.

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
