# A constraint solver that stops wasted search with *tendency*, not *logic*

*Short intro to sabori_csp, a hobby FlatZinc/CP solver I wrote in C++. Full write-up with all the numbers and code links at the bottom.*

sabori_csp is a constraint solver with a standard skeleton — backtracking + propagation + restarts + activity-based variable selection + NoGood learning. What's distinctive is *how* it cuts wasted search, and it's easiest to state by contrast with LCG (lazy clause generation, à la Chuffed):

> **LCG stops wasted search with *logic*: it learns sound clauses from conflicts and deductively prunes regions it provably never revisits. sabori_csp stops it with *tendency*: it feeds the same conflict information mainly into activity, steering the heuristic tendency of variable selection away from bad regions.**

It's deliberately lightweight — **no LCG, no implication-graph analysis, no per-propagator explanations** — and it leans on cheap, self-tuning heuristics instead. Here's what it actually does, and (because I A/B-tested all of it) what carries its weight.

## What does the work

I put each component behind an env-var toggle and A/B-tested it across the MiniZinc Challenge suite over multiple random seeds, judged by objective value (the exact tables and methodology are in the full write-up). Three things move the needle:

- **Weak NoGood learning, surprisingly effective.** On conflict, sabori just records the decision trail as one NoGood — much weaker than 1-UIP or LCG. Turning it off consistently hurts search across every seed. The interesting part: splitting its effect shows the gain comes **mainly from activity, not pruning** — dropping only the NoGood-driven *activity bumps* hurts much more than dropping only the *clause pruning*. The learned conflict mostly works by reinforcing activity and reshaping the search *tendency*. (That's exactly the "tendency, not logic" idea from the title.)
- **A bandit over the variable-ordering mix.** "Activity-first vs MRV-first" is a 5-arm multi-armed bandit, re-sampled every restart. It doesn't beat the best fixed heuristic — it *robustly avoids the worst* one per problem. Insurance for when you can't pick the right heuristic up front.
- **One-hot channeling presolve.** FlatZinc explodes integers into one-hot boolean groups; sabori fuses each `b_i ⇔ (x==v_i)` group back into a single channel constraint. A modest win on average, and on some problems it cuts search effort dramatically.

So the engine is: a weak learner and a bandit, both feeding *activity*, plus a model-shrinking presolve. Cheap parts, steering by tendency.

## What I measured that *didn't* pan out

I'd rather report this than hide it — and for a solver, knowing what's redundant is as useful as knowing what works:

- A **Bloom-fingerprint NoGood-overlap tiebreak** for variable selection: almost always a no-op — on the vast majority of problems it changes nothing. Activity already carries that signal, so the tiebreak rarely gets a turn.
- **Per-constraint *structural* conflict-blame** — clever code where Circuit's `occupier_`, AllDifferent's value pool, etc. point at the culprit at variable granularity (a "poor man's explanation": explanation-quality blame, heuristic-only so it's allowed to be unsound). I was proud of it. Against the dumb generic version it shows no measurable gain — the win/loss even flips from seed to seed. The specialization buys nothing → future work.
- A **pseudo-gradient value hint**: negative on average, but it splits by problem — helps design/assignment, backfires on resource-coupled scheduling. Useless always-on, but a fine slot in a parallel portfolio.

The pattern is consistent and, I think, the real lesson: **the foundation that grows activity works; the cleverness piled on top of live activity mostly doesn't, because activity is already the dominant signal.** Same reason the "tendency, not logic" framing holds — sabori does prune deductively, but what's actually cutting waste is mostly the tendency side.

## Read more

The full write-up walks all 8 components with complete tables, code references, and the honest negatives (including a community-detection feature that's diagnostics-only): **[search-algorithm-explained-en.md](search-algorithm-explained-en.md)** (original Japanese: [search-algorithm-explained.md](search-algorithm-explained.md)).

Code: <https://github.com/tofjw/sabori_csp>. FlatZinc-compliant, entered in the MiniZinc Challenge 2025 (results pending — I'll write those up honestly whichever way they land). Happy to be told I've reinvented something with a name I don't know — if there's an established name for this idea, I'd genuinely like to hear it.
