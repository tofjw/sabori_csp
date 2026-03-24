#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <vector>

namespace sabori_csp {

// ============================================================================
// TimeTablingPropagator
// ============================================================================

bool TimeTablingPropagator::propagate_impl(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    bool direct, bool& changed)
{
    // var_ids layout: [s0..sn-1, d0..dn-1, r0..rn-1, capacity]
    size_t cap_id = var_ids[3 * n];
    // For overload check and pruning, use cap_max (most permissive).
    // For capacity lower-bound propagation, use profile peak.
    int64_t cap_max = direct ? model.variable(cap_id)->max() : model.var_max(cap_id);

    // Step 1: Build profile events from mandatory parts
    std::vector<Event> events;
    events.reserve(2 * n);

    for (size_t i = 0; i < n; ++i) {
        int64_t dur_min = model.var_min(var_ids[n + i]);
        int64_t req_min = model.var_min(var_ids[2 * n + i]);
        if (dur_min < 0 || req_min <= 0) continue;

        int64_t est = model.var_min(var_ids[i]);          // earliest start
        int64_t lst = model.var_max(var_ids[i]);          // latest start

        // Gecode 互換: dur=0 のポイントタスクも開始時点でリソースを消費する。
        // FlatZinc 仕様上は s[i] <= t < s[i]+d[i] なので dur=0 は空区間だが、
        // Gecode/Chuffed はポイントタスクをカウントする。MiniZinc Challenge の
        // 問題はこのセマンティクスを前提としているため、互換性のため合わせる。
        if (dur_min == 0) {
            if (est == lst) {
                events.push_back({est, req_min});
                events.push_back({est + 1, -req_min});
            }
            continue;
        }

        int64_t ect = est + dur_min;                       // earliest completion

        // Mandatory part exists when LST < ECT
        if (lst < ect) {
            events.push_back({lst, req_min});
            events.push_back({ect, -req_min});
        }
    }

    // No mandatory parts → no pruning possible
    if (events.empty()) return true;

    // Sort events by time (stable for ties)
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.time < b.time; });

    // Step 2: Build piecewise-constant profile
    // profile[k] = {time, usage} meaning usage from time to next time
    struct ProfileEntry {
        int64_t time;
        int64_t usage;
    };
    std::vector<ProfileEntry> profile;
    profile.reserve(events.size() + 1);

    int64_t current_usage = 0;
    int64_t profile_peak = 0;
    size_t ei = 0;
    while (ei < events.size()) {
        int64_t t = events[ei].time;
        while (ei < events.size() && events[ei].time == t) {
            current_usage += events[ei].delta;
            ++ei;
        }
        if (current_usage > profile_peak) profile_peak = current_usage;
        // Overload check: even max capacity cannot accommodate
        if (current_usage > cap_max) return false;
        profile.push_back({t, current_usage});
    }

    // Propagate capacity lower bound: capacity >= profile_peak
    int64_t cap_min = direct ? model.variable(cap_id)->min() : model.var_min(cap_id);
    if (profile_peak > cap_min) {
        if (direct) {
            if (!model.variable(cap_id)->remove_below(
                    static_cast<Domain::value_type>(profile_peak)))
                return false;
        } else {
            model.enqueue_set_min(cap_id, static_cast<Domain::value_type>(profile_peak));
        }
        changed = true;
    }

    // Step 3: Pruning — for each task, check if profile forces EST/LST changes
    for (size_t j = 0; j < n; ++j) {
        size_t s_id = var_ids[j];
        size_t d_id = var_ids[n + j];
        size_t r_id = var_ids[2 * n + j];

        int64_t dur_min = model.var_min(d_id);
        int64_t req_min = model.var_min(r_id);
        if (dur_min < 0 || req_min <= 0) continue;
        // dur=0 ポイントタスクは Step 1 のプロファイルでカウント済み。
        // EST/LST の刈り込みは duration > 0 のタスクのみ対象。
        if (dur_min == 0) continue;
        if (direct && model.variable(s_id)->is_assigned()) continue;
        if (!direct && model.is_instantiated(s_id)) continue;

        int64_t est = direct ? model.variable(s_id)->min() : model.var_min(s_id);
        int64_t lst = direct ? model.variable(s_id)->max() : model.var_max(s_id);

        // Forward pruning: find earliest feasible start
        // Task j runs [start, start + dur_min) using req_min resource
        // For each profile interval overlapping [est, est + dur_min),
        // if profile_usage + req_min > cap_max, push est forward.
        int64_t new_est = est;
        for (size_t k = 0; k < profile.size(); ++k) {
            int64_t p_start = profile[k].time;
            int64_t p_end = (k + 1 < profile.size()) ? profile[k + 1].time
                                                       : p_start + 1;
            int64_t p_usage = profile[k].usage;

            // Skip intervals that end before new_est
            if (p_end <= new_est) continue;
            // Stop if interval starts after task would end
            if (p_start >= new_est + dur_min) break;

            // Check if task j's mandatory part contributes to this interval
            int64_t j_lst = lst;
            int64_t j_ect = est + dur_min;
            int64_t overlap_usage = p_usage;
            if (j_lst < j_ect && p_start < j_ect && p_end > j_lst) {
                // Task j has mandatory part in this interval, subtract its contribution
                overlap_usage -= req_min;
            }

            if (overlap_usage + req_min > cap_max) {
                // Cannot start at new_est — must start after p_end
                new_est = p_end;
            }
        }

        if (new_est > lst) return false;  // No feasible start

        if (new_est > est) {
            if (direct) {
                if (!model.variable(s_id)->remove_below(
                        static_cast<Domain::value_type>(new_est)))
                    return false;
            } else {
                model.enqueue_set_min(s_id, static_cast<Domain::value_type>(new_est));
            }
            changed = true;
        }

        // Backward pruning: find latest feasible start
        int64_t new_lst = lst;
        for (int k = static_cast<int>(profile.size()) - 1; k >= 0; --k) {
            int64_t p_start = profile[k].time;
            int64_t p_end = (static_cast<size_t>(k) + 1 < profile.size())
                                ? profile[k + 1].time
                                : p_start + 1;
            int64_t p_usage = profile[k].usage;

            // Skip intervals starting after new_lst + dur_min
            if (p_start >= new_lst + dur_min) continue;
            // Stop if interval ends before new_lst
            if (p_end <= new_lst) break;

            // Subtract task j's own mandatory part contribution
            int64_t j_lst_orig = lst;
            int64_t j_ect_orig = est + dur_min;
            int64_t overlap_usage = p_usage;
            if (j_lst_orig < j_ect_orig && p_start < j_ect_orig && p_end > j_lst_orig) {
                overlap_usage -= req_min;
            }

            if (overlap_usage + req_min > cap_max) {
                // Cannot start at new_lst — must start before p_start
                new_lst = p_start - dur_min;
            }
        }

        // Re-read est in case it was updated
        int64_t current_est = (new_est > est) ? new_est : est;
        if (new_lst < current_est) return false;  // No feasible start

        if (new_lst < lst) {
            if (direct) {
                if (!model.variable(s_id)->remove_above(
                        static_cast<Domain::value_type>(new_lst)))
                    return false;
            } else {
                model.enqueue_set_max(s_id, static_cast<Domain::value_type>(new_lst));
            }
            changed = true;
        }
    }

    return true;
}

PresolveResult TimeTablingPropagator::propagate_presolve(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    CumulativeEngineStats& stats)
{
    ++stats.call_count;
    bool changed = false;
    if (!propagate_impl(model, n, var_ids, true, changed)) {
        ++stats.contradiction_count;
        return PresolveResult::Contradiction;
    }
    if (changed) ++stats.reduction_count;
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool TimeTablingPropagator::propagate_search(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    CumulativeEngineStats& stats)
{
    ++stats.call_count;
    bool changed = false;
    if (!propagate_impl(model, n, var_ids, false, changed)) {
        ++stats.contradiction_count;
        return false;
    }
    if (changed) ++stats.reduction_count;
    return true;
}

// ============================================================================
// TTEFPropagator
// ============================================================================

void TTEFPropagator::build_tasks(const Model& model, size_t n,
                                  const std::vector<size_t>& var_ids)
{
    tasks_.clear();
    tasks_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        int64_t dur = model.var_min(var_ids[n + i]);
        int64_t req = model.var_min(var_ids[2 * n + i]);
        if (dur < 0 || req <= 0) continue;
        // dur=0 ポイントタスクは TimeTabling で処理。TTEF のエネルギー計算では
        // energy=dur*req=0 となり刈り込みに寄与しないためスキップ。
        if (dur == 0) continue;

        int64_t est = model.var_min(var_ids[i]);
        int64_t lst = model.var_max(var_ids[i]);
        tasks_.push_back({i, est, lst, est + dur, lst + dur, dur, req, dur * req});
    }
}

void TTEFPropagator::build_profile()
{
    // Build mandatory-part profile from tasks_
    struct Event { int64_t time; int64_t delta; };
    std::vector<Event> events;
    events.reserve(2 * tasks_.size());

    for (const auto& t : tasks_) {
        // Mandatory part exists when LST < ECT
        if (t.lst < t.ect) {
            events.push_back({t.lst, t.req});
            events.push_back({t.ect, -t.req});
        }
    }

    profile_.clear();
    if (events.empty()) {
        // Empty profile — single entry at 0 with usage 0
        profile_.push_back({0, 0});
        prefix_energy_.assign(1, 0);
        return;
    }

    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.time < b.time; });

    int64_t current_usage = 0;
    size_t ei = 0;
    while (ei < events.size()) {
        int64_t t = events[ei].time;
        while (ei < events.size() && events[ei].time == t) {
            current_usage += events[ei].delta;
            ++ei;
        }
        profile_.push_back({t, current_usage});
    }

    // Build prefix energy sum: prefix_energy_[k] = sum of profile[j].usage * (profile[j+1].time - profile[j].time) for j < k
    prefix_energy_.resize(profile_.size());
    prefix_energy_[0] = 0;
    for (size_t k = 1; k < profile_.size(); ++k) {
        int64_t width = profile_[k].time - profile_[k - 1].time;
        prefix_energy_[k] = prefix_energy_[k - 1] + profile_[k - 1].usage * width;
    }
}

int64_t TTEFPropagator::profile_integral(int64_t lo, int64_t hi) const
{
    if (lo >= hi || profile_.empty()) return 0;

    // Find first profile entry with time > lo
    // We use profile_ as a step function: profile_[k].usage is active from profile_[k].time to profile_[k+1].time
    // Before profile_[0].time, usage is 0; after last entry, usage is profile_.back().usage (should be 0 for valid profiles)

    int64_t integral = 0;

    for (size_t k = 0; k < profile_.size(); ++k) {
        int64_t seg_start = profile_[k].time;
        int64_t seg_end = (k + 1 < profile_.size()) ? profile_[k + 1].time : seg_start;
        int64_t usage = profile_[k].usage;

        // Clip to [lo, hi)
        int64_t a = std::max(seg_start, lo);
        int64_t b = std::min(seg_end, hi);
        if (a < b) {
            integral += usage * (b - a);
        }
    }

    return integral;
}

bool TTEFPropagator::forward_pass(
    Model& model, const std::vector<size_t>& var_ids,
    int64_t cap_max, bool direct, bool& changed)
{
    // Sort tasks by LCT ascending for the forward pass
    std::vector<size_t> order(tasks_.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        return tasks_[a].lct < tasks_[b].lct;
    });

    // For each prefix Theta = {order[0..k]}, check energy bounds
    for (size_t k = 0; k < order.size(); ++k) {
        // Compute Theta = {order[0..k]}
        int64_t R = tasks_[order[k]].lct;
        int64_t L = tasks_[order[0]].est;
        int64_t energy_theta = 0;

        for (size_t i = 0; i <= k; ++i) {
            const auto& t = tasks_[order[i]];
            if (t.est < L) L = t.est;
            energy_theta += t.energy;
        }

        if (R <= L) continue;

        int64_t tt_energy = profile_integral(L, R);
        int64_t capacity_area = (R - L) * cap_max;
        int64_t free = capacity_area - tt_energy;

        // Compute theta_cp: mandatory energy of Theta tasks within [L, R)
        int64_t theta_cp = 0;
        for (size_t i = 0; i <= k; ++i) {
            const auto& t = tasks_[order[i]];
            if (t.lst < t.ect) {
                int64_t a = std::max(t.lst, L);
                int64_t b = std::min(t.ect, R);
                if (a < b) theta_cp += t.req * (b - a);
            }
        }

        int64_t extra_theta = energy_theta - theta_cp;

        // Overload check
        if (extra_theta > free) return false;

        // Pruning: for tasks NOT in Theta
        for (size_t j_idx = k + 1; j_idx < order.size(); ++j_idx) {
            const auto& tj = tasks_[order[j_idx]];
            size_t orig_j = tj.idx;
            size_t s_id = var_ids[orig_j];

            // Skip if task j cannot overlap with [L, R)
            if (tj.est >= R || tj.lct <= L) continue;

            // Energy of task j in [L, R) when starting at est_j
            int64_t overlap = std::min(R, tj.ect) - std::max(L, tj.est);
            if (overlap <= 0) continue;
            overlap = std::min(overlap, tj.dur);
            int64_t energy_j_in_LR = tj.req * overlap;

            // Subtract mandatory contribution already in profile
            int64_t j_mandatory_in_LR = 0;
            if (tj.lst < tj.ect) {
                int64_t a = std::max(tj.lst, L);
                int64_t b = std::min(tj.ect, R);
                if (a < b) j_mandatory_in_LR = tj.req * (b - a);
            }

            int64_t extra_j = energy_j_in_LR - j_mandatory_in_LR;
            int64_t free_for_j = free;

            if (extra_theta + extra_j > free_for_j) {
                int64_t slack = free_for_j - extra_theta;
                if (slack < 0) return false;

                int64_t new_est = R - slack / tj.req;
                if (new_est > tj.est && new_est <= tj.lst) {
                    if (direct) {
                        if (!model.variable(s_id)->remove_below(
                                static_cast<Domain::value_type>(new_est)))
                            return false;
                    } else {
                        model.enqueue_set_min(s_id, static_cast<Domain::value_type>(new_est));
                    }
                    changed = true;
                }
            }
        }
    }

    return true;
}

bool TTEFPropagator::backward_pass(
    Model& model, const std::vector<size_t>& var_ids,
    int64_t cap_max, bool direct, bool& changed)
{
    // Sort tasks by EST descending for the backward pass
    std::vector<size_t> order(tasks_.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        return tasks_[a].est > tasks_[b].est;
    });

    for (size_t k = 0; k < order.size(); ++k) {
        int64_t L = tasks_[order[k]].est;
        int64_t R = tasks_[order[0]].lct;
        int64_t energy_theta = 0;

        for (size_t i = 0; i <= k; ++i) {
            const auto& t = tasks_[order[i]];
            if (t.lct > R) R = t.lct;
            energy_theta += t.energy;
        }

        if (R <= L) continue;

        int64_t tt_energy = profile_integral(L, R);
        int64_t capacity_area = (R - L) * cap_max;
        int64_t free = capacity_area - tt_energy;

        int64_t theta_cp = 0;
        for (size_t i = 0; i <= k; ++i) {
            const auto& t = tasks_[order[i]];
            if (t.lst < t.ect) {
                int64_t a = std::max(t.lst, L);
                int64_t b = std::min(t.ect, R);
                if (a < b) theta_cp += t.req * (b - a);
            }
        }

        int64_t extra_theta = energy_theta - theta_cp;

        if (extra_theta > free) return false;

        // Pruning: for tasks NOT in Theta, push LST down
        for (size_t j_idx = k + 1; j_idx < order.size(); ++j_idx) {
            const auto& tj = tasks_[order[j_idx]];
            size_t orig_j = tj.idx;
            size_t s_id = var_ids[orig_j];

            // Skip if task j cannot overlap with [L, R)
            if (tj.est >= R || tj.lct <= L) continue;

            // Energy of task j in [L, R) when ending at lct_j
            int64_t overlap = std::min(R, tj.lct) - std::max(L, tj.lst);
            if (overlap <= 0) continue;
            overlap = std::min(overlap, tj.dur);
            int64_t energy_j_in_LR = tj.req * overlap;

            int64_t j_mandatory_in_LR = 0;
            if (tj.lst < tj.ect) {
                int64_t a = std::max(tj.lst, L);
                int64_t b = std::min(tj.ect, R);
                if (a < b) j_mandatory_in_LR = tj.req * (b - a);
            }

            int64_t extra_j = energy_j_in_LR - j_mandatory_in_LR;
            int64_t free_for_j = free;

            if (extra_theta + extra_j > free_for_j) {
                int64_t slack = free_for_j - extra_theta;
                if (slack < 0) return false;

                int64_t new_lct = L + slack / tj.req;
                int64_t new_lst = new_lct - tj.dur;
                if (new_lst < tj.lst && new_lst >= tj.est) {
                    if (direct) {
                        if (!model.variable(s_id)->remove_above(
                                static_cast<Domain::value_type>(new_lst)))
                            return false;
                    } else {
                        model.enqueue_set_max(s_id, static_cast<Domain::value_type>(new_lst));
                    }
                    changed = true;
                }
            }
        }
    }

    return true;
}

bool TTEFPropagator::propagate_impl(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    bool direct, bool& changed)
{
    build_tasks(model, n, var_ids);
    if (tasks_.size() < 2) return true;

    size_t cap_id = var_ids[3 * n];
    int64_t cap_max = direct ? model.variable(cap_id)->max() : model.var_max(cap_id);

    build_profile();

    if (!forward_pass(model, var_ids, cap_max, direct, changed)) return false;

    // Rebuild tasks and profile with updated bounds before backward pass
    if (changed) {
        build_tasks(model, n, var_ids);
        if (tasks_.size() < 2) return true;
        build_profile();
    }

    if (!backward_pass(model, var_ids, cap_max, direct, changed)) return false;

    return true;
}

PresolveResult TTEFPropagator::propagate_presolve(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    CumulativeEngineStats& stats)
{
    ++stats.call_count;
    bool changed = false;
    if (!propagate_impl(model, n, var_ids, true, changed)) {
        ++stats.contradiction_count;
        return PresolveResult::Contradiction;
    }
    if (changed) ++stats.reduction_count;
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool TTEFPropagator::propagate_search(
    Model& model, size_t n, const std::vector<size_t>& var_ids,
    CumulativeEngineStats& stats)
{
    ++stats.call_count;
    bool changed = false;
    if (!propagate_impl(model, n, var_ids, false, changed)) {
        ++stats.contradiction_count;
        return false;
    }
    if (changed) ++stats.reduction_count;
    return true;
}

// ============================================================================
// CumulativeConstraint
// ============================================================================

CumulativeConstraint::CumulativeConstraint(
    std::vector<VariablePtr> starts,
    std::vector<VariablePtr> durations,
    std::vector<VariablePtr> requirements,
    VariablePtr capacity)
    : Constraint()
    , n_(starts.size())
{
    // var_ids_ = [s0..sn-1, d0..dn-1, r0..rn-1, capacity]
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(3 * n_ + 1);
    for (auto& v : starts)       all_vars.push_back(std::move(v));
    for (auto& v : durations)    all_vars.push_back(std::move(v));
    for (auto& v : requirements) all_vars.push_back(std::move(v));
    all_vars.push_back(std::move(capacity));

    var_ids_ = extract_var_ids(all_vars);

    // Register engines: TimeTabling first, then TTEF
    engines_.push_back(std::make_unique<TimeTablingPropagator>());
    engines_.push_back(std::make_unique<TTEFPropagator>());
    engine_stats_.resize(engines_.size());
}

std::string CumulativeConstraint::name() const {
    return "fzn_cumulative";
}

std::vector<std::string> CumulativeConstraint::engine_names() const {
    std::vector<std::string> names;
    names.reserve(engines_.size());
    for (const auto& e : engines_) {
        names.push_back(e->engine_name());
    }
    return names;
}

void CumulativeConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                          double* activity, double activity_inc,
                                          bool& need_rescale, std::mt19937& rng) const {
    // デフォルトの bump（instantiated 変数に均等加算）
    Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);

    // trigger 変数のタスクインデックスを特定
    // var_ids_ レイアウト: [s0..sn-1, d0..dn-1, r0..rn-1, capacity]
    size_t trigger_task = SIZE_MAX;
    for (size_t idx = 0; idx < 3 * n_; ++idx) {
        if (var_ids_[idx] == trigger_var_idx) {
            trigger_task = idx % n_;
            break;
        }
    }
    if (trigger_task == SIZE_MAX) return;

    // trigger タスクの mandatory part [lst, ect) を計算
    int64_t t_dur = model.var_min(var_ids_[n_ + trigger_task]);
    int64_t t_req = model.var_min(var_ids_[2 * n_ + trigger_task]);
    int64_t t_est = model.var_min(var_ids_[trigger_task]);
    int64_t t_lst = model.var_max(var_ids_[trigger_task]);
    int64_t t_ect = t_est + t_dur;
    bool trigger_has_mandatory = (t_dur > 0 && t_req > 0 && t_lst < t_ect);

    // 同一タスクの兄弟変数 + mandatory part が重なる他タスクの変数を bump
    double inc = activity_inc / 3.0;

    // 同一タスクの兄弟
    size_t trigger_vars[3] = {
        var_ids_[trigger_task],
        var_ids_[n_ + trigger_task],
        var_ids_[2 * n_ + trigger_task]
    };
    for (size_t sid : trigger_vars) {
        if (sid != trigger_var_idx) {
            bump_variable_activity(activity, sid, inc, need_rescale, rng);
        }
    }

    // mandatory part が重なる他タスク
    if (trigger_has_mandatory) {
        for (size_t i = 0; i < n_; ++i) {
            if (i == trigger_task) continue;
            int64_t di = model.var_min(var_ids_[n_ + i]);
            int64_t ri = model.var_min(var_ids_[2 * n_ + i]);
            if (di <= 0 || ri <= 0) continue;
            int64_t ei = model.var_min(var_ids_[i]);
            int64_t li = model.var_max(var_ids_[i]);
            int64_t ecti = ei + di;
            // mandatory part [li, ecti) が trigger の [t_lst, t_ect) と重なるか
            if (li < ecti && li < t_ect && ecti > t_lst) {
                bump_variable_activity(activity, var_ids_[i], inc, need_rescale, rng);
                bump_variable_activity(activity, var_ids_[n_ + i], inc, need_rescale, rng);
                bump_variable_activity(activity, var_ids_[2 * n_ + i], inc, need_rescale, rng);
            }
        }
    }
}

bool CumulativeConstraint::run_all_engines(Model& model) {
    for (size_t i = 0; i < engines_.size(); ++i) {
        if (!engines_[i]->propagate_search(model, n_, var_ids_, engine_stats_[i])) {
            return false;
        }
    }
    return true;
}

PresolveResult CumulativeConstraint::presolve(Model& model) {
    bool any_changed = false;
    for (size_t i = 0; i < engines_.size(); ++i) {
        auto result = engines_[i]->propagate_presolve(model, n_, var_ids_, engine_stats_[i]);
        if (result == PresolveResult::Contradiction) return PresolveResult::Contradiction;
        if (result == PresolveResult::Changed) any_changed = true;
    }
    return any_changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool CumulativeConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool CumulativeConstraint::on_instantiate(
    Model& model, int save_point,
    size_t var_idx, size_t internal_var_idx,
    Domain::value_type value,
    Domain::value_type prev_min, Domain::value_type prev_max)
{
    if (!Constraint::on_instantiate(model, save_point, var_idx,
                                     internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    if (!has_uninstantiated(model)) {
        return on_final_instantiate(model);
    }

    return run_all_engines(model);
}

bool CumulativeConstraint::on_final_instantiate(const Model& model) {
    // All variables assigned: verify resource usage at every time point
    int64_t cap = model.value(var_ids_[3 * n_]);

    // Collect task intervals
    struct Task {
        int64_t start, end, req;
    };
    std::vector<Task> tasks;
    tasks.reserve(n_);

    for (size_t i = 0; i < n_; ++i) {
        int64_t s = model.value(var_ids_[i]);
        int64_t d = model.value(var_ids_[n_ + i]);
        int64_t r = model.value(var_ids_[2 * n_ + i]);
        if (d <= 0 || r <= 0) continue;
        tasks.push_back({s, s + d, r});
    }

    // Sweep-line check
    struct Event {
        int64_t time;
        int64_t delta;
    };
    std::vector<Event> events;
    events.reserve(2 * tasks.size());
    for (const auto& t : tasks) {
        events.push_back({t.start, t.req});
        events.push_back({t.end, -t.req});
    }
    // 同じ時刻では end event (delta<0) を先に処理する
    // cumulative は半開区間 [start, start+dur) なので、t で終了するタスクと
    // t で開始するタスクは同時に走っていない
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) {
                  if (a.time != b.time) return a.time < b.time;
                  return a.delta < b.delta;  // negative (end) before positive (start)
              });

    int64_t usage = 0;
    for (const auto& e : events) {
        usage += e.delta;
        if (usage > cap) return false;
    }

    return true;
}

bool CumulativeConstraint::on_set_min(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/)
{
    return run_all_engines(model);
}

bool CumulativeConstraint::on_set_max(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/)
{
    return run_all_engines(model);
}

void CumulativeConstraint::rewind_to(int /*save_point*/) {
    // Stateless — nothing to rewind
}

}  // namespace sabori_csp
