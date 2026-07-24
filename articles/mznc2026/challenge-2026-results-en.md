# MiniZinc Challenge 2026 results: 9th of 26 entries, right below native CDCL

I entered sabori_csp, my homegrown FlatZinc/CP solver, in the MiniZinc Challenge 2026. Summing the per-instance results of the free category, it placed **9th of 26 entries**. Directly above sits pumpkin, a native-CDCL solver: on solution quality (the Incomplete score) sabori is level with it, and on the proof-counting Complete score it falls behind. And it wiped out completely on two problems. More than the placement itself, this pattern — where it draws level and where it wipes out — is the interesting test of the [full write-up](search-algorithm-explained-en.md)'s thesis, "stop wasted search with tendency, not logic." So that's what this post breaks down.

## Where the data comes from, and how to read it

The [official results page](https://www.minizinc.org/challenge/2026/results/) shows only the top 3, but the per-instance CSV is available, so I aggregated it myself (`articles/mznc2026/results.csv`; 20 problems × 5 instances × 26 entries). There are three scores:

- **Complete**: round-robin comparison of every solver pair per instance. The better objective value takes the point; on equal objectives, proving optimality does. The tables below show the sums.
- **Incomplete**: the same round-robin, but ignoring optimality proofs — solution quality only.
- **Area**: the integral of the objective over time. Reaching good solutions earlier makes it smaller (lower is better).

One caveat. The rankings here are "the sum of the score columns in this CSV," which is not the official medal reckoning. The medal count excludes organizer-entered solvers, among other rules — but the comparisons I care about are exactly vs cp-sat and vs gecode, so this post counts all entries, ineligible ones included.

## Overall standings

| Rank | Solver | Complete | Incomplete | Area (M) |
|---|---|---|---|---|
| 1 | or-tools cp-sat | 2124.0 | 1950.7 | 16.70 |
| 2 | huub (difference logic) | 1803.5 | 1687.4 | 28.42 |
| 3 | huub | 1790.9 | 1674.6 | 28.60 |
| 4 | cp_optimizer | 1768.0 | 1686.6 | 33.92 |
| 5 | picatsat | 1658.3 | 1590.0 | 31.51 |
| 6 | chuffed | 1634.6 | 1559.3 | 31.96 |
| 7 | gurobi 12 | 1539.3 | 1444.6 | 40.10 |
| 8 | pumpkin | 1366.2 | 1333.3 | 44.07 |
| **9** | **sabori_csp** | **1312.8** | **1334.0** | **43.53** |
| 10 | cplex | 1137.5 | 1080.4 | 53.28 |
| 11 | highs | 1084.5 | 1095.0 | 59.51 |
| 12 | scip | 1053.9 | 1074.6 | 59.26 |
| 13 | qiuqi-mixsolver | 1010.0 | 1239.0 | 58.89 |
| 14 | choco-solver (cp) | 976.8 | 1039.6 | 57.31 |
| 15 | or-tools cp-sat-ls | 927.3 | 1125.1 | 60.61 |
| 16 | choco-solver (cp+lcg) | 902.7 | 994.3 | 60.86 |
| 17 | thornbill | 827.1 | 817.6 | 65.58 |
| 18 | sicstus prolog | 784.4 | 779.5 | 67.89 |
| 19 | gecode (fd) | 747.5 | 702.1 | 76.07 |
| 20 | gecode scheduling portfolio | 710.3 | 697.4 | 75.26 |
| 21 | jacop | 689.0 | 709.5 | 68.92 |
| 22 | yuck | 661.1 | 760.5 | 74.76 |
| 23 | cbc | 483.5 | 509.8 | 84.52 |
| 24 | crusher | 347.2 | 391.3 | 90.54 |
| 25 | nucs (fd) | 288.1 | 330.7 | 93.28 |
| 26 | atlantis | 205.4 | 226.9 | 101.26 |

Look at the names. The top eight are SAT/LCG solvers (two huub entries, picatsat, chuffed, pumpkin) and commercial ones (cp_optimizer, gurobi), with cp-sat at the top — every one of them carries strong learning or mathematical programming. sabori sits at **the tail of the learning column, right below pumpkin** — and above every non-learning CP solver (choco's cp configuration, gecode, jacop) and the mid-tier MIP solvers (cplex, highs, scip). That position is the third-party answer to the write-up's question: how far does a lightweight solver without LCG carry?

## The gap to pumpkin

pumpkin, directly above, is a native-CDCL solver written in Rust — in learning strength, close to the opposite pole from sabori, whose learning is just the conjunction of decision literals. Here's how that difference landed:

| Metric | sabori_csp | pumpkin |
|---|---|---|
| Complete | 1312.8 | 1366.2 |
| Incomplete | **1334.0** | 1333.3 |
| Area (M) | **43.53** | 44.07 |
| Instances with a solution | **82** / 100 | 76 / 100 |
| Optimality proofs | 28 | **34** |

53 points behind on Complete; level on Incomplete once proofs are ignored (0.7 points is just a tie). sabori reached a solution on 6 more instances; pumpkin proved optimality 6 more times. **sabori reaches further; pumpkin closes more of what it reaches.** The write-up's contrast — tendency competes on getting to good solutions, proofs are logic's job — became literal numbers in a third-party measurement.

One caveat: the Incomplete tie is partly an artifact of the round-robin scoring. Counting head-to-head on objective values alone, sabori goes 21–32–47 against pumpkin — a losing record (21–27 even excluding the two wipeouts). sabori takes big points on the problems it wins and bleeds small losses elsewhere. Put differently, its per-problem variance is high.

## Per-problem scores

| Problem | Kind | sabori | Rank | cp-sat | pumpkin | chuffed |
|---|---|---|---|---|---|---|
| nside | MAX | **103.4** | **1** | 86.1 | 60.1 | 94.8 |
| gbac | MIN | 93.3 | 5 | 102.3 | 75.3 | 31.2 |
| saeling | MIN | 85.7 | 5 | 116.2 | 59.7 | 45.7 |
| tdtsp | MIN | 98.0 | 6 | 108.3 | 74.0 | 67.5 |
| vrplc | MIN | 98.0 | 6 | 115.5 | 65.5 | 110.0 |
| spot5 | MIN | 87.5 | 7 | 122.3 | 35.0 | 60.0 |
| nonogram | SAT | 74.7 | 7 | 97.0 | 33.0 | 109.6 |
| gcc-benchmark | SAT | 71.3 | 7 | 79.8 | 70.4 | 87.1 |
| filters | MIN | 84.1 | 8 | 102.6 | 77.2 | 84.1 |
| rect-euler | MIN | 80.5 | 8 | 110.6 | 107.0 | 78.7 |
| workforce-alloc | MIN | 75.4 | 10 | 110.0 | 87.4 | 87.4 |
| sdn-chain | MIN | 66.9 | 12 | 103.5 | 102.8 | 105.5 |
| atp-stage2 | MIN | 55.9 | 14 | 110.3 | 90.5 | 54.9 |
| warehouse | MIN | 42.0 | 15 | 114.0 | 0.0 | 86.0 |
| zephyrus-2016 | MIN | 56.9 | 15 | 99.8 | 95.0 | 105.2 |
| kitchen | MAX | 56.1 | 16 | 114.7 | 93.9 | 84.4 |
| community-detection-2021 | MAX | 57.5 | 16 | 104.3 | 89.6 | 91.6 |
| surface-based-tsp | MIN | 25.7 | 18 | 99.0 | 58.8 | 91.8 |
| multiple-constant-multiplication | MIN | 0.0 | 22 | 111.7 | 32.6 | 98.9 |
| orthorio | MIN | 0.0 | 22 | 116.1 | 58.5 | 60.4 |

Three highlights.

**nside: first place overall.** The only problem where sabori beat cp-sat, with optimality proved on 2 of 5 instances (one of them, HARD_1000_100, proved after 412 seconds — oddly the EASY instances went unproved, so the names and the actual difficulty don't seem to line up). Given the overall objective-value record vs cp-sat is 5 wins, 55 losses, 40 ties, winning a whole problem against it is a serious outlier.

**The problems where sabori ranks high smell of routing and assignment.** tdtsp and vrplc are routing problems, gbac is curriculum assignment, spot5 is the well-known satellite-scheduling benchmark. The direction matches Section 7's observation in the write-up (helps on design/assignment problems), but these are hand labels applied after the fact — the trend looks consistent, and that's all I'll claim.

**The bottom is consistently lost.** kitchen, community-detection, zephyrus, and atp-stage2 rank low across all 5 instances each — real distance, not seed luck. Also one ERR on warehouse's wlp22 (uninvestigated; the only error out of 100 instances).

## The two wipeouts

multiple-constant-multiplication and orthorio: all 5 instances UNK, meaning not a single solution in 1200 seconds. surface-based-tsp also went 3 of 5 UNK.

The interesting part is who did solve them. The solvers that solved all 5 instances are, on mcm: chuffed, both huub entries, picatsat, cp-sat, and cp_optimizer; on orthorio: both huub entries, picatsat, and cp-sat. **All SAT/LCG or commercial.** The non-learning CP solvers weren't strictly at zero, but close: jacop picked up 3 instances on mcm, gecode 1 on orthorio. Even pumpkin, native CDCL, managed only 2–3. sabori's wipeout doesn't look like an individual bug so much as the weak-learning camp sinking together.

The write-up's scope note said: on classes where strong learning is decisive — problems where 1-UIP/LCG prune by orders of magnitude — tendency alone won't reach, and that wasn't measured. These two problems look like exactly that unmeasured region, materialized in third-party data. mcm is reportedly a circuit-design problem (decompose constant multiplications into adds and shifts), a tight combinatorial structure where learning pays. That said, the CSV can't distinguish "search never got there" from "fell over earlier, in flattening or memory." The verdict waits until I reproduce it locally.

## Takeaways and homework

How the reckoning came out:

- The self-measured hypothesis — tendency control can compete with learning solvers on solution quality — got external confirmation, in the form of the Incomplete tie with pumpkin and the 82-vs-76 solution reach. Falling behind on proofs was also as predicted.
- The strong-learning-dominant class the write-up declared unmeasured turned out to exist: mcm and orthorio, where the weak-learning camp sank together.
- Against the non-learning CP solvers (gecode, jacop, choco cp), sabori won on every total. Within this field, the design bet — lightweight, no LCG — paid for itself.

Two pieces of homework: reproduce mcm, orthorio, and surface-based-tsp locally and sort out whether the wipeout is search or the infrastructure ahead of it; and squash the warehouse wlp22 ERR.

Source: [results.csv](results.csv) (per-instance results from the official site). Full write-up: [search-algorithm-explained-en.md](search-algorithm-explained-en.md). Original Japanese of this post: [challenge-2026-results.md](challenge-2026-results.md).
