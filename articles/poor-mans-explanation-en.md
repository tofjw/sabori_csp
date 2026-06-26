# A CP solver without lazy clause generation: the "poor man's explanation" trick

*Notes on a hobby FlatZinc/CP solver I built in C++ (sabori_csp). This is the short, English version of a longer Japanese write-up that compares the whole search stack against CDCL/LCG/MiniZinc solvers. Here I want to focus on the one idea I think is genuinely worth stealing.*

## The setup

If you've looked at modern constraint solvers, you know the rough spectrum:

- **CDCL SAT solvers** (MiniSat, Glucose) learn a clause from each conflict by walking the implication graph back to a 1-UIP cut. The variables in that learned clause are exactly the ones you bump in VSIDS.
- **Lazy Clause Generation** (Chuffed) gets the best of CP and SAT: every propagator is taught to produce an *explanation* — a logically sound clause justifying each value it removed. Those explanations feed a real CDCL learning engine, so you get precise, variable-level conflict analysis on top of global constraints.

LCG is the "right" answer, and it's powerful. But it's expensive: you have to write a sound explanation routine for *every* propagator, and "sound" is load-bearing — if an explanation is wrong, your learning is wrong and the solver becomes unsound.

I didn't want to pay that cost. sabori_csp does **no implication-graph analysis and no LCG**. Conflict learning is deliberately dumb: when search hits a conflict, it just records the current decision trail (the list of branching literals) as a single NoGood. That's weaker than 1-UIP — the learned NoGood isn't minimal and only mentions decision variables — but it's uniform across every constraint and costs almost nothing to implement.

Here's the problem that creates. **If your learning is weak, your variable-selection heuristic has to carry the search.** And good variable selection means answering the classic question well: *when a conflict happens, who's to blame?* That's exactly the question LCG explanations answer precisely — and I'd just thrown explanations away.

## The trick: separate "blame" from "learning"

The insight is that conflict analysis is used for *two different things*, and they have *different correctness requirements*:

1. **Learning** (producing clauses you'll propagate later) — must be **sound**. A wrong clause corrupts the search.
2. **Heuristic blame** (deciding whose activity to bump) — does **not** need to be sound. If you bump slightly the wrong variable, you waste a little search effort. That's it. No correctness consequence.

LCG fuses these: it uses the *same* sound explanation for both. That's elegant, but it means the whole thing has to clear the high bar of soundness.

sabori_csp splits them. Learning stays dumb and uniform (decision-trail NoGoods). But **blame is computed per-constraint, from each constraint's own internal data structures** — and because it's *only* used to bump activity, it's allowed to be cheap and approximate and even wrong. I call it a **poor man's explanation**: explanation-quality blame assignment without the explanation machinery.

Concretely, `Constraint::bump_activity` is `virtual`, and each global constraint overrides it to point at the likely culprits using state it already maintains:

**Base constraint** — only blame variables whose bounds *actually moved* since presolve, and weight by domain size:

```cpp
// count vars whose [min,max] changed from the presolved bounds
size_t n = 0;
for (size_t vid : var_ids_)
    if (model.var_min(vid) != model.presolve_min(vid)
     || model.var_max(vid) != model.presolve_max(vid)) n++;

for (size_t vid : var_ids_)
    if (model.var_min(vid) != model.presolve_min(vid)
     || model.var_max(vid) != model.presolve_max(vid)) {
        double inc = activity_inc / n / model.var_size(vid); // smaller domain → heavier
        bump_variable_activity(activity, vid, inc, need_rescale, rng);
    }
```

Variables that never moved are presumed innocent. The `1/domain_size` factor is a fail-first flavor: a near-pinned variable gets blamed harder.

**AllDifferent** — when a fixed value `val` causes the wipeout, blame only the variables still competing for `val` (its conflict set), found via the value pool:

```cpp
if (pool_.consumed(val)) {
    // distribute only among vars that can still take val
    for (size_t vid : var_ids_)
        if (model.contains(vid, val)) /* bump this one */;
}
```

**Circuit** — the constraint already tracks which variable is assigned to each value in an `occupier_` array, so it can find the *exact* colliding partner in O(1) and split the blame 50/50 between the trigger and its partner:

```cpp
const double inc = 0.5 * activity_inc;
bump_variable_activity(activity, trigger_var_idx, inc, ...);
size_t occ = occupier_[trigger_val - base_offset_];   // O(1) — who else took this value
if (occ != SIZE_MAX) bump_variable_activity(activity, var_ids_[occ], inc, ...);
```

**Linear equality** — look at *which* bound (lower or upper) was violated and distribute to the variables that, given their coefficient signs, contributed to that side.

None of these is a sound explanation. The Circuit one is nearly exact; the base one is a crude "things that moved" heuristic. But they're all O(scope) or better, they reuse structures the propagator already has, and they assign blame at *variable* granularity — which is strictly more informative than what dom/wdeg gives you.

## Why this is the interesting part

The usual comparison table for "who do you blame on a conflict":

| Method | Blame granularity | Soundness required? |
|---|---|---|
| dom/wdeg | per-constraint scalar (whole scope, uniform) | no |
| VSIDS / ABS | learned-clause vars / domain-shrunk vars | (learning side handles it) |
| LCG explanation | per-variable, precise | **yes** (it's reused for learning) |
| **poor man's explanation** | **per-variable, constraint-specific** | **no** (heuristic-only) |

dom/wdeg is sound-free but coarse — it blames the entire constraint scope uniformly. LCG is precise but pays for soundness everywhere. The poor man's explanation sits in the gap: **per-variable precision like LCG, zero soundness cost like dom/wdeg**, by exploiting the fact that a heuristic is allowed to lie.

The design rationale was appealing: *learn cheaply and uniformly, then spend the saved complexity on smart, structure-aware blame* — weak learning and rich blame as two sides of one coin. Whether that second half actually pays off is an empirical question, and I finally measured it (see below). Spoiler: the generic version is the part that stands; the per-constraint structural cleverness on top doesn't earn its keep.

## The honest caveats

I'm not claiming a new algorithm. Bumping variables, fail-first domain weighting, and per-constraint conflict accounting are all old ideas (VSIDS, ABS, dom/wdeg, Boussemart et al. 2004). What I think is uncommon is the *deliberate decoupling*: a lightweight CP solver with **no** explanations that still does per-variable, per-constraint-structure blame, precisely *because* it refuses to make that blame sound.

Whether it's actually *better* than just doing LCG properly — I don't have the evidence to claim that, and I doubt it on hard structured problems where Chuffed's real learning dominates. The honest pitch is narrower: *if you're writing a CP solver and don't want to implement LCG, you don't have to fall back to coarse dom/wdeg blame. You can get most of the localization for free from structures your propagators already maintain.*

And I ran the ablation on the narrower question — does the per-constraint *structural* blame actually beat a generic version? The blame layer has three levels: **none** (no constraint-side bump at all), **base** (the generic poor man's explanation: bump the variables whose bounds moved, weight by domain size), and **structural** (the per-constraint overrides above — Circuit's `occupier_`, AllDifferent's pool, etc.). On 38 MiniZinc Challenge 2023–24 problems, 30s, **5 seeds**, judged by objective value (190 problem×seed data points):

- **structural vs base: 37–41–112 (net −4), and the sign flips from seed to seed.** The per-constraint structural specialization does *not* measurably beat the generic version.
- base vs none comes out net −7 the other way, but that's inside the same noise band — not enough to claim the localization itself is a clear win either.

A single seed briefly had me convinced the structural version was actively *worse* (9–6 for base); five seeds erased the gap. So the honest, narrow finding: **the cheap generic localization is a reasonable baseline; the extra per-constraint structure I layered on top buys nothing measurable.** The `occupier_`/pool-based blame is kept but unproven — future work. What survives is the concept, not the elaboration: heuristic-only, unsound, cheap conflict localization without LCG.

There's a matching negative result from the same project, for balance: I also built explicit community detection (VIG + label propagation) hoping the structure would guide search. It didn't help on any benchmark — activity heuristics apparently learn the same locality implicitly. It's now a diagnostics-only feature.

## If you want the rest

The full Japanese article walks the whole search stack the same way — bandit-learned variable-ordering mix, a Bloom-fingerprint NoGood-overlap tiebreak, inner/outer adaptive restarts, and a pseudo-gradient graft onto branch-and-bound — always framed as "standard skeleton + a thin self-tuning layer," with the same try-to-falsify-myself tone.

Code and the long write-up: <https://github.com/tofjw/sabori_csp>. The solver is FlatZinc-compliant and I entered it in the MiniZinc Challenge 2026 (results pending — I'll write those up honestly whichever way they land).

Happy to be told I've reinvented something with a name I don't know — that's half the reason I'm posting.
