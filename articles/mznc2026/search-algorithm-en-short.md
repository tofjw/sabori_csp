# A constraint solver that stops wasted search with *tendency*, not *logic*

*Short intro to sabori_csp, a hobby FlatZinc/CP solver I wrote in C++. Full write-up with all the numbers and code links at the bottom.*

sabori_csp is a constraint solver with a standard skeleton — backtracking + propagation + restarts + activity-based variable selection + NoGood learning. What's distinctive is *how* it cuts wasted search, and it's easiest to state by contrast with the mainstream LCG/CDCL line (lazy clause generation, à la Chuffed) — which turns conflicts into sound logical constraints and prunes deductively:

> **LCG stops wasted search with *logic*: it learns sound clauses from conflicts and deductively prunes regions it provably never revisits. sabori_csp stops it with *tendency*: it feeds the same conflict information into the variable-selection heuristic — a conflict-directed lean toward recently-conflicted variables — steering away from bad regions.**

It's deliberately lightweight — **no LCG, no implication-graph analysis, no per-propagator explanations** — and it leans on cheap, self-tuning heuristics instead. Here's what it actually does, and (because I A/B-tested all of it) what carries its weight.

## What does the work

I put each component behind an env-var toggle and A/B-tested it across the MiniZinc Challenge suite over multiple random seeds, judged by objective value (the exact tables and methodology are in the full write-up). What moves the needle:

- **Variable selection itself is the foundation, and two mechanisms split the labor by search phase.** Right after a backtrack, a **conflict-directed primary criterion (a Last-Conflict-style mechanism)** picks the most-recently-conflicted variable first — on its own the single most effective component. As search then descends normally, **activity drives the selections**. temporal picks the restart point; activity drives the descent.
- **That activity is fed by the weak NoGood learning.** On conflict, sabori just records the decision trail as one NoGood — much weaker than 1-UIP or LCG. Even so, the gain comes **mainly from activity, not pruning** — dropping the NoGood's *activity bumps* hurts far more than dropping its *clause pruning*. The learned conflict mostly works by reinforcing activity and reshaping the search *tendency* (the title's "tendency, not logic," measured).
- **A bandit over the variable-ordering mix (the activity axis).** "Activity-first vs MRV-first," re-sampled every restart. It doesn't beat the best fixed heuristic — it *robustly avoids the worst* one per problem.
- **One-hot channeling presolve.** FlatZinc explodes integers into one-hot boolean groups; sabori fuses each `b_i ⇔ (x==v_i)` group back into a single channel constraint. A modest win on average, and on some problems it cuts search effort dramatically.

So the engine is: conflict-directed variable selection (a Last-Conflict-style primary criterion + activity-driven descent), fed by the weak NoGood and tuned by the bandit, plus a model-shrinking presolve. Cheap parts, steering by tendency.

## What I measured that *didn't* pan out

I'd rather report this than hide it — and for a solver, knowing what's redundant is as useful as knowing what works:

- A **Bloom-fingerprint NoGood-overlap tiebreak** for variable selection: almost always a no-op — on the vast majority of problems it changes nothing. Activity already carries that signal, so the tiebreak rarely gets a turn.
- **Per-constraint *structural* conflict-blame** — each constraint uses its own structure (Circuit's `occupier_`, AllDifferent's value pool, etc.) to point at the culprit at variable granularity, a "poor man's explanation" (heuristic-only, so it's allowed to be unsound). Against the dumb generic version (which just blames every moved variable uniformly) it shows no measurable gain. The specialization buys nothing → future work.
- **The adaptive restart control.** sabori's restart is an inner-outer double loop, and I tried to shrink the outer bound in response to how search was going (NoGood-pruning progress × depth growth). It didn't work: the shrink side had been dead since a refactor, and even after fixing it there was no effect. The plain nested restart (+ reset on improvement) runs fine on its own, so no harm done.
- A **pseudo-gradient value hint**: negative on average, but it splits by problem — helps design/assignment, backfires on resource-coupled scheduling. Useless always-on, but a fine slot in a parallel portfolio.

The pattern is consistent and, I think, the real lesson: **the conflict-directed selection foundation (the Last-Conflict primary criterion + activity-driven descent) works; the cleverness piled on top mostly doesn't, because the foundation already decides search.** Same reason the "tendency, not logic" framing holds — sabori does prune deductively, but what's actually cutting waste is mostly the tendency side. One scope note: sabori deliberately carries only the weakest learning, so this measures "how far tendency alone carries you once logic is minimized" — not that deductive pruning is generally redundant. On classes where strong learning (1-UIP / LCG) is decisive, tendency alone won't reach, and that's not what's measured here.

## Read more

The full write-up walks all 8 components with complete tables, code references, and the honest negatives (including a community-detection feature that's diagnostics-only): **[search-algorithm-explained-en.md](search-algorithm-explained-en.md)** (original Japanese: [search-algorithm-explained.md](search-algorithm-explained.md)).

Code: <https://github.com/tofjw/sabori_csp>. FlatZinc-compliant, entered in the MiniZinc Challenge 2026 (results pending — I'll write those up honestly whichever way they land). None of the individual parts are new — the full write-up cites the prior work (Last Conflict, bandit-based variable selection, activity's implicit capture of structure, etc.). If I've overlooked a relevant reference, please point me to it.
