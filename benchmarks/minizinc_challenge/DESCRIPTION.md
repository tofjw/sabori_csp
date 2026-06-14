# sabori_csp — System Description (MiniZinc Challenge 2026)

## Authors

- **T. Fujiwara** &lt;tttfjw@gmail.com&gt; — *[affiliation]*

## Overview

sabori_csp is a C++17 constraint solver that reads FlatZinc and solves CSP /
optimization problems by depth-first search with restarts, conflict-driven
nogood learning, and activity-based variable ordering. The architecture is
deliberately simple — single centralised event queue, sparse-set domains
with a bounds-only fast path, central trail — so that more effort can go
into the search-control mechanisms described below. None of the headline
techniques (CDCL-style nogood learning, VSIDS, Luby-style restarts, TTEF
cumulative reasoning, ...) are new on their own; the parts that are
unusual in our experience are the way activities are *initialised* and
*credited*, the inter-restart **improvement probe**, and the use of an
on-line **community analysis** to drive search-control hyperparameters.

## Search

Branching is per-variable: bisect-on-bounds when the live domain has
&gt; 8 values, enumerate-values otherwise, with the value order biased by the
last incumbent ("solution-guided") and an optional **pseudo-gradient hint**
described below.

Variable selection is VSIDS-style with two non-standard additions:

- **Mode reward.** We maintain two selection modes, *activity-first*
  (highest VSIDS) and *value-first* (smallest domain, activity tiebreaker).
  Each solution rewards the mode that produced it; the mode used for the
  next decision is sampled with weight proportional to cumulative reward,
  with a small exploration probability. This recovers a domain-size policy
  on hard combinatorial structure while letting activity dominate where
  conflicts are informative.
- **Per-constraint `init_activity` / `bump_activity`.** Each global
  constraint contributes initial activity to its variables that reflects
  its own structure (e.g. for `int_lin_eq` the weight on `x_i` is
  `|c_i| / |D_i|` normalised so the constraint contributes 1.0 in total;
  for `all_different` it is approximately `(n-1) · log(d/(d-1))` per
  variable). On conflict the bump is also constraint-aware: linear
  constraints only bump variables whose bound moves *since presolve* are
  on the side overshooting the target, with coarse-grained variables that
  individually exceed the overshoot filtered out, and a small jitter on
  the credit. Initial activities are rescaled so the maximum is 100, so
  that early VSIDS bumps are not drowned out.

Restarts use an adaptive outer/inner cycle: the inner conflict limit grows
geometrically inside an outer cycle; the outer cycle's length is widened
or shrunk between cycles depending on whether nogood pruning fired and
whether `max_depth` grew during the cycle.

### Pseudo-Gradient Value Hint (optimization)

A solution-based phase-saving scheme on its own ("try the value the variable
took in the incumbent first") is well known. We extend it with a *directional*
component derived from how the incumbent moved between successive improving
solutions.

After presolve we mark the *gradient-eligible* set: decision variables that are
not defined-by-others, not eliminated, are scheduled for enumerate branching
(i.e. small enough domain that we will pick a concrete value at the decision)
and whose presolve range is wider than 3. These are typically the
"macro" variables of the model — assignment indices, configuration choices,
ordering positions — where successive improving solutions tend to drift in a
consistent direction.

Whenever an improving solution `S_k` is found, we compute a sign per eligible
variable from the difference with the previous improving solution `S_{k-1}`:

```
g[v] = sign(S_k[v] − S_{k-1}[v])  ∈ {−1, 0, +1}
```

We then pick one eligible variable `v` uniformly at random; if `g[v] ≠ 0`
and the incumbent value is not already at the presolve-side extreme in that
direction, we record `(v, sign, ref = S_k[v])` as the gradient hint for the
next cycle. The hint is reset on every restart cycle.

When the search next decides on `v`, the value enumerator first
shuffles the live domain, then promotes a randomly-chosen value strictly on
the gradient side of `ref` to the front (followed by `ref` itself, then the
remaining shuffled values). The effect is a single-variable, single-step
randomised hill-climbing nudge: we try to extend the trend that produced the
last two improvements while keeping the rest of the value order unbiased so
that the search can recover if the trend was a coincidence.

Because the hint targets exactly one variable per cycle and is sampled
randomly from the eligible set, the pseudo-gradient does not collapse the
search to a single trajectory — it provides a soft preference that interacts
with the activity-based variable order rather than overriding it. The
gradient state is recomputed at the end of each `improvement probe` step,
not before, so probe decisions do not consume the hint.

## Nogood Learning

Conflict analysis walks the decision trail and emits a nogood with the
usual 2-watched-literal scheme. We use three specialised watcher kinds:

- **Equality watches** for `x = v` literals.
- **Bound watches** for `x ≤ v` / `x ≥ v` literals, which fire on
  `set_max` / `set_min` events rather than on instantiation. This lets
  bound propagation drive nogood-derived unit propagation without
  waiting for variables to become singleton.
- A **512-bit per-decision Bloom filter** records which variables have
  touched any active nogood since the last decision; this becomes a
  cheap tiebreaker in variable selection ("prefer variables connected
  to recent conflicts").

Unit nogoods learned at root are folded back as permanent domain
restrictions so that they survive subsequent restarts.

## Improvement Probe (optimization)

Between outer restart cycles, in optimization mode, the solver runs a
short *speculative probe*: it temporarily adds the bound
`obj ≤ obj_ub − ⌈(obj_ub − obj_lb) / 20⌉` (symmetric for maximization)
and runs a budgeted search (default fail-limit 10). On SAT the new
incumbent is committed; on UNSAT the tightened lower side is added as
a permanent bound. This produces large objective jumps on problems
whose feasible set has a sparse objective spectrum and is the main
reason sabori_csp keeps closing on the optimum on long-tail instances.
The post-probe propagation is checked at root level only — recent fix
work has tightened the soundness of this step, in particular the energy
budget passed to the cumulative TTEF prune.

## Cumulative

`fzn_cumulative` is implemented with two engines run in sequence:

1. **TimeTabling** with a piecewise-constant mandatory-part profile and
   forward/backward sweeps that prune EST/LST and propagate the capacity
   lower bound from the profile peak.
2. **TTEF (Time-Tabling Edge-Finding)** with a Θ-prefix sweep ordered by
   LCT (forward) / EST (backward). The energy budget used to compute the
   new EST of a non-Θ task `j` is `slack + j_mandatory_in_LR`, not just
   `slack`: the time-table profile already accounts for `j`'s own
   mandatory contribution in `[L,R)`, and that contribution must be
   added back when `j` is being moved.

## Other Custom Globals

- `int_one_hot_channel` aggregates a fan-out of
  `int_eq_reif(x, k_i, b_i)` with constant `k_i` into a single channel
  constraint that does exactly-one on the boolean array and forces
  `x = k_i` whenever `b_i` becomes true. This significantly reduces the
  number of reified comparisons that the propagation engine has to wake.
- `int_lin_eq` performs bidirectional bound propagation in *O(n)* per
  event by maintaining `current_fixed_sum`, `min_rem_potential`,
  `max_rem_potential` differentially under a save-point trail.

## Community Analysis (optional, `-c`)

After presolve we build the Variable Interaction Graph (variables that
co-occur in a constraint) and run Label Propagation to detect
communities. During search we track per-restart locality (fraction of
decisions in the previous decision's community), cross- vs intra-
community propagations, and per-community decision counts. Modularity
Q ≈ 0 ("globally coupled") triggers a more exploratory restart regime;
Q ≥ 0.3 ("clearly clustered") favours longer outer cycles to exploit
structure. The analysis is off by default in the challenge entry but the
mechanism is present.

## Implementation

C++17, ~30 kLOC core + Bison/Flex FlatZinc parser. No third-party
runtime dependencies (Catch2 is used for tests only). Build is
CMake-only. The challenge image is built on `minizinc/mznc2026:latest`
following the standard `.msc` + `Preferences.json` layout.
