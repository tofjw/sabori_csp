#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cstring>

namespace sabori_csp {

// ============================================================================
// DisjunctiveConstraint — bitmap-based implementation with Compulsory Parts
// ============================================================================

DisjunctiveConstraint::DisjunctiveConstraint(
    std::vector<VariablePtr> starts,
    std::vector<VariablePtr> durations,
    bool strict)
    : Constraint(std::vector<VariablePtr>())
    , n_(starts.size())
    , strict_(strict)
{
    // vars_ = [s0, s1, ..., sn-1, d0, d1, ..., dn-1]
    vars_.reserve(2 * n_);
    for (auto& v : starts)    vars_.push_back(std::move(v));
    for (auto& v : durations) vars_.push_back(std::move(v));

    // Compute offset and horizon
    if (n_ > 0) {
        int64_t min_start = vars_[0]->min();
        int64_t max_end   = vars_[0]->max() + vars_[n_]->max();
        for (size_t i = 1; i < n_; ++i) {
            min_start = std::min(min_start, vars_[i]->min());
            int64_t end_i = vars_[i]->max() + vars_[n_ + i]->max();
            if (end_i > max_end) max_end = end_i;
        }
        offset_  = static_cast<int>(min_start);
        horizon_ = static_cast<int>(max_end - min_start);
    } else {
        offset_  = 0;
        horizon_ = 0;
    }

    size_t num_words = (horizon_ > 0) ? static_cast<size_t>((horizon_ + 63) / 64) : 0;
    timeline_.assign(num_words, 0);
    cp_lo_.assign(n_, 0);
    cp_hi_.assign(n_, 0);

    update_var_ids();
    check_initial_consistency();
}

std::string DisjunctiveConstraint::name() const {
    return strict_ ? "fzn_disjunctive_strict" : "fzn_disjunctive";
}

std::vector<VariablePtr> DisjunctiveConstraint::variables() const {
    return vars_;
}

std::optional<bool> DisjunctiveConstraint::is_satisfied() const {
    for (const auto& var : vars_) {
        if (!var->is_assigned()) return std::nullopt;
    }
    for (size_t i = 0; i < n_; ++i) {
        auto si = vars_[i]->assigned_value().value();
        auto di = vars_[n_ + i]->assigned_value().value();
        for (size_t j = i + 1; j < n_; ++j) {
            auto sj = vars_[j]->assigned_value().value();
            auto dj = vars_[n_ + j]->assigned_value().value();
            if (!strict_ && (di == 0 || dj == 0)) continue;
            if (!(si + di <= sj || sj + dj <= si)) return false;
        }
    }
    return true;
}

// ---------- Task helpers ----------

bool DisjunctiveConstraint::task_fully_assigned(size_t task) const {
    return vars_[task]->is_assigned() && vars_[n_ + task]->is_assigned();
}

int DisjunctiveConstraint::task_start(size_t task) const {
    return static_cast<int>(vars_[task]->assigned_value().value());
}

int DisjunctiveConstraint::task_dur(size_t task) const {
    return static_cast<int>(vars_[n_ + task]->assigned_value().value());
}

int DisjunctiveConstraint::task_dur_min(size_t task) const {
    return static_cast<int>(vars_[n_ + task]->min());
}

// ---------- Bit operations ----------

bool DisjunctiveConstraint::check_conflict(int start, int len) const {
    if (len <= 0) return false;
    int end = start + len;
    int first_word = start / 64;
    int last_word  = (end - 1) / 64;
    int start_bit  = start % 64;

    if (first_word == last_word) {
        uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
        mask <<= start_bit;
        return (timeline_[first_word] & mask) != 0;
    }

    // First word
    uint64_t first_mask = ~0ULL << start_bit;
    if (timeline_[first_word] & first_mask) return true;

    // Middle words
    for (int w = first_word + 1; w < last_word; ++w) {
        if (timeline_[w]) return true;
    }

    // Last word
    int end_bit = (end - 1) % 64;
    uint64_t last_mask = (end_bit == 63) ? ~0ULL : ((1ULL << (end_bit + 1)) - 1);
    return (timeline_[last_word] & last_mask) != 0;
}

int DisjunctiveConstraint::count_set_bits(int start, int len) const {
    if (len <= 0) return 0;
    int end = start + len;
    int first_word = start / 64;
    int last_word  = (end - 1) / 64;
    int start_bit  = start % 64;

    if (first_word == last_word) {
        uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
        mask <<= start_bit;
        return __builtin_popcountll(timeline_[first_word] & mask);
    }

    int count = 0;

    // First word
    uint64_t first_mask = ~0ULL << start_bit;
    count += __builtin_popcountll(timeline_[first_word] & first_mask);

    // Middle words
    for (int w = first_word + 1; w < last_word; ++w) {
        count += __builtin_popcountll(timeline_[w]);
    }

    // Last word
    int end_bit = (end - 1) % 64;
    uint64_t last_mask = (end_bit == 63) ? ~0ULL : ((1ULL << (end_bit + 1)) - 1);
    count += __builtin_popcountll(timeline_[last_word] & last_mask);

    return count;
}

int DisjunctiveConstraint::count_free_bits(int start, int len) const {
    return len - count_set_bits(start, len);
}

bool DisjunctiveConstraint::check_conflict_excluding(int start, int len, size_t exclude_task) const {
    int cp_lo = cp_lo_[exclude_task];
    int cp_hi = cp_hi_[exclude_task];

    // No CP for this task -> standard check
    if (cp_lo >= cp_hi) {
        return check_conflict(start, len);
    }

    int end = start + len;

    // Check [start, min(end, cp_lo))
    if (start < cp_lo) {
        int check_end = std::min(end, cp_lo);
        if (check_conflict(start, check_end - start)) return true;
    }

    // Check [max(start, cp_hi), end)
    if (cp_hi < end) {
        int check_start = std::max(start, cp_hi);
        if (check_conflict(check_start, end - check_start)) return true;
    }

    return false;
}

int DisjunctiveConstraint::find_first_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const {
    if (dur <= 0) return lo;
    int cp_lo = cp_lo_[exclude_task];
    int cp_hi = cp_hi_[exclude_task];
    bool has_cp = (cp_lo < cp_hi);

    for (int pos = lo; pos <= hi;) {
        if (pos + dur > horizon_) return -1;
        if (!check_conflict_excluding(pos, dur, exclude_task)) return pos;

        // Find next candidate position
        if (has_cp && pos >= cp_lo && pos < cp_hi) {
            // Inside CP range: linear scan (CP is typically small)
            ++pos;
        } else {
            // Outside CP: can use find_next_zero to skip
            int next = find_next_zero(pos + 1);
            // If next lands inside CP, that's fine - bits there are ours
            if (has_cp && next >= cp_lo && next < cp_hi) {
                pos = next;
            } else {
                pos = (next > pos + 1) ? next : pos + 1;
            }
        }
    }
    return -1;
}

int DisjunctiveConstraint::find_last_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const {
    if (dur <= 0) return hi;
    int cp_lo = cp_lo_[exclude_task];
    int cp_hi = cp_hi_[exclude_task];
    bool has_cp = (cp_lo < cp_hi);

    for (int pos = hi; pos >= lo;) {
        if (pos + dur <= horizon_ && !check_conflict_excluding(pos, dur, exclude_task))
            return pos;

        // Skip occupied blocks efficiently
        if (has_cp && pos >= cp_lo && pos < cp_hi) {
            // Inside own CP range: linear scan (CP is typically small)
            --pos;
        } else {
            int prev = find_prev_zero(pos - 1);
            if (has_cp && prev >= cp_lo && prev < cp_hi) {
                pos = prev;
            } else {
                pos = (prev < pos - 1) ? prev : pos - 1;
            }
        }
    }
    return -1;
}

void DisjunctiveConstraint::set_bits_direct(int start, int len) {
    if (len <= 0) return;
    int end = start + len;
    int first_word = start / 64;
    int last_word  = (end - 1) / 64;
    int start_bit  = start % 64;

    if (first_word == last_word) {
        uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
        mask <<= start_bit;
        timeline_[first_word] |= mask;
        return;
    }

    timeline_[first_word] |= (~0ULL << start_bit);
    for (int w = first_word + 1; w < last_word; ++w) {
        timeline_[w] = ~0ULL;
    }
    int end_bit = (end - 1) % 64;
    uint64_t last_mask = (end_bit == 63) ? ~0ULL : ((1ULL << (end_bit + 1)) - 1);
    timeline_[last_word] |= last_mask;
}

void DisjunctiveConstraint::ensure_dirty_marked(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

void DisjunctiveConstraint::set_bits(Model& model, int save_point, int start, int len) {
    if (len <= 0) return;
    ensure_dirty_marked(model, save_point);

    int end = start + len;
    int first_word = start / 64;
    int last_word  = (end - 1) / 64;
    int start_bit  = start % 64;

    if (first_word == last_word) {
        uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
        mask <<= start_bit;
        trail_.push_back({save_point, {first_word, timeline_[first_word]}});
        timeline_[first_word] |= mask;
        return;
    }

    // First word
    trail_.push_back({save_point, {first_word, timeline_[first_word]}});
    timeline_[first_word] |= (~0ULL << start_bit);

    // Middle words
    for (int w = first_word + 1; w < last_word; ++w) {
        trail_.push_back({save_point, {w, timeline_[w]}});
        timeline_[w] = ~0ULL;
    }

    // Last word
    int end_bit = (end - 1) % 64;
    uint64_t last_mask = (end_bit == 63) ? ~0ULL : ((1ULL << (end_bit + 1)) - 1);
    trail_.push_back({save_point, {last_word, timeline_[last_word]}});
    timeline_[last_word] |= last_mask;
}

int DisjunctiveConstraint::find_next_zero(int from) const {
    int word_idx = from / 64;
    int bit_idx  = from % 64;
    int num_words = static_cast<int>(timeline_.size());

    if (word_idx >= num_words) return from;

    // Check remaining bits in the current word
    uint64_t masked = timeline_[word_idx] >> bit_idx;
    if (~masked != 0) {
        return from + __builtin_ctzll(~masked);
    }

    // Scan subsequent full words
    for (int w = word_idx + 1; w < num_words; ++w) {
        if (~timeline_[w] != 0) {
            return w * 64 + __builtin_ctzll(~timeline_[w]);
        }
    }

    return num_words * 64;
}

int DisjunctiveConstraint::find_prev_zero(int from) const {
    if (from < 0) return from;
    int word_idx = from / 64;
    int bit_idx  = from % 64;
    int num_words = static_cast<int>(timeline_.size());

    if (word_idx >= num_words) {
        // Beyond timeline — all bits are implicitly zero
        return from;
    }

    // Check bits [0..bit_idx] in the current word
    // Mask out bits above bit_idx
    uint64_t mask = (bit_idx == 63) ? ~0ULL : ((1ULL << (bit_idx + 1)) - 1);
    uint64_t masked = timeline_[word_idx] & mask;
    if (~masked & mask) {
        // There's a zero bit in [0..bit_idx] of this word
        // Find the highest zero bit: invert, mask, find highest set bit
        uint64_t zeros = ~masked & mask;
        return word_idx * 64 + 63 - __builtin_clzll(zeros);
    }

    // Scan preceding words
    for (int w = word_idx - 1; w >= 0; --w) {
        if (~timeline_[w] != 0) {
            return w * 64 + 63 - __builtin_clzll(~timeline_[w]);
        }
    }

    return -1;  // No zero bit found
}

// ---------- Compulsory Part ----------

bool DisjunctiveConstraint::update_compulsory_part(Model& model, int save_point, size_t task) {
    int dur = static_cast<int>(model.var_min(var_ids_[n_ + task]));
    if (dur <= 0) return true;
    if (!strict_ && dur == 0) return true;

    int est = static_cast<int>(model.var_min(var_ids_[task])) - offset_;
    int lst = static_cast<int>(model.var_max(var_ids_[task])) - offset_;

    int new_lo = lst;
    int new_hi = est + dur;

    // No compulsory part
    if (new_lo >= new_hi) return true;

    int old_lo = cp_lo_[task];
    int old_hi = cp_hi_[task];

    // No change
    if (new_lo == old_lo && new_hi == old_hi) return true;

    // Save old state to trail
    ensure_dirty_marked(model, save_point);
    cp_trail_.push_back({save_point, {task, old_lo, old_hi}});

    // Set expanded bits (only the new parts)
    if (old_lo >= old_hi) {
        // No previous CP: set entire [new_lo, new_hi)
        if (check_conflict_excluding(new_lo, new_hi - new_lo, task)) return false;
        set_bits(model, save_point, new_lo, new_hi - new_lo);
    } else {
        // CP expanded: set only the new parts
        // Left expansion: [new_lo, old_lo)
        if (new_lo < old_lo) {
            if (check_conflict_excluding(new_lo, old_lo - new_lo, task)) return false;
            set_bits(model, save_point, new_lo, old_lo - new_lo);
        }
        // Right expansion: [old_hi, new_hi)
        if (new_hi > old_hi) {
            if (check_conflict_excluding(old_hi, new_hi - old_hi, task)) return false;
            set_bits(model, save_point, old_hi, new_hi - old_hi);
        }
    }

    cp_lo_[task] = new_lo;
    cp_hi_[task] = new_hi;

    return true;
}

bool DisjunctiveConstraint::update_compulsory_part_direct(size_t task) {
    int dur = task_dur_min(task);
    if (dur <= 0) return true;
    if (!strict_ && dur == 0) return true;

    int est = static_cast<int>(vars_[task]->min()) - offset_;
    int lst = static_cast<int>(vars_[task]->max()) - offset_;

    int new_lo = lst;
    int new_hi = est + dur;

    // No compulsory part
    if (new_lo >= new_hi) return true;

    // Check for conflict (exclude own CP bits, but during presolve CP was 0,0)
    if (check_conflict_excluding(new_lo, new_hi - new_lo, task)) return false;

    set_bits_direct(new_lo, new_hi - new_lo);
    cp_lo_[task] = new_lo;
    cp_hi_[task] = new_hi;

    return true;
}

// ---------- Edge-Finding ----------

bool DisjunctiveConstraint::edge_finding(Model& model, bool direct) {
    // Collect tasks with positive dur_min
    struct TaskInfo {
        size_t idx;
        int est;
        int lst;
        int dur;
        int ect;  // est + dur
        int lct;  // lst + dur
    };
    std::vector<TaskInfo> tasks;
    tasks.reserve(n_);

    for (size_t i = 0; i < n_; ++i) {
        auto d_id = var_ids_[n_ + i];
        int dur = direct ? task_dur_min(i) : static_cast<int>(model.var_min(d_id));
        if (dur <= 0) continue;
        if (!strict_ && dur == 0) continue;

        int est, lst;
        if (direct) {
            est = static_cast<int>(vars_[i]->min()) - offset_;
            lst = static_cast<int>(vars_[i]->max()) - offset_;
        } else {
            est = static_cast<int>(model.var_min(var_ids_[i])) - offset_;
            lst = static_cast<int>(model.var_max(var_ids_[i])) - offset_;
        }
        tasks.push_back({i, est, lst, dur, est + dur, lst + dur});
    }

    size_t m = tasks.size();
    if (m <= 1) return true;

    // Forward pass (NOT-FIRST / push EST right): sort by LCT ascending
    std::sort(tasks.begin(), tasks.end(),
              [](const TaskInfo& a, const TaskInfo& b) { return a.lct < b.lct; });

    for (size_t k = 0; k < m; ++k) {
        int R = tasks[k].lct;
        int L = tasks[0].est;
        int energy_theta = 0;
        int max_ect_theta = 0;
        for (size_t i = 0; i <= k; ++i) {
            L = std::min(L, tasks[i].est);
            energy_theta += tasks[i].dur;
            max_ect_theta = std::max(max_ect_theta, tasks[i].ect);
        }

        // TTEF: compute external occupation in [L, R)
        int interval_len = R - L;
        int total_occupied = count_set_bits(L, interval_len);
        int theta_cp_in_LR = 0;
        for (size_t i = 0; i <= k; ++i) {
            int clo = std::max(L, cp_lo_[tasks[i].idx]);
            int chi = std::min(R, cp_hi_[tasks[i].idx]);
            if (clo < chi) theta_cp_in_LR += chi - clo;
        }
        int external_occupied = total_occupied - theta_cp_in_LR;

        // Overload check (TTEF-enhanced)
        if (energy_theta + external_occupied > interval_len) return false;

        // Push tasks outside Theta
        for (size_t j = k + 1; j < m; ++j) {
            auto j_s_id = var_ids_[tasks[j].idx];
            if (direct ? vars_[tasks[j].idx]->is_assigned() : model.is_instantiated(j_s_id)) continue;

            int dur_j = tasks[j].dur;
            int est_j = tasks[j].est;

            // Subtract j's own CP from external_occupied
            int j_cp_in_LR = 0;
            {
                int clo = std::max(L, cp_lo_[tasks[j].idx]);
                int chi = std::min(R, cp_hi_[tasks[j].idx]);
                if (clo < chi) j_cp_in_LR = chi - clo;
            }
            int ext_for_j = external_occupied - j_cp_in_LR;

            // NOT-FIRST condition (TTEF-enhanced)
            int effective_L = std::min(L, est_j);
            if (effective_L + energy_theta + dur_j + ext_for_j > R) {
                int new_est = L + energy_theta + ext_for_j;
                // Simple DP: j must come after all Theta tasks
                new_est = std::max(new_est, max_ect_theta);
                if (new_est > est_j) {
                    auto new_min = static_cast<Domain::value_type>(new_est + offset_);
                    if (direct) {
                        if (!vars_[tasks[j].idx]->remove_below(new_min)) return false;
                    } else {
                        model.enqueue_set_min(j_s_id, new_min);
                    }
                }
            }
        }
    }

    // Backward pass (NOT-LAST / push LST left): sort by EST descending
    std::sort(tasks.begin(), tasks.end(),
              [](const TaskInfo& a, const TaskInfo& b) { return a.est > b.est; });

    for (size_t k = 0; k < m; ++k) {
        int L = tasks[k].est;
        int R = tasks[0].lct;
        int energy_theta = 0;
        int min_lst_theta = tasks[0].lst;
        for (size_t i = 0; i <= k; ++i) {
            R = std::max(R, tasks[i].lct);
            energy_theta += tasks[i].dur;
            min_lst_theta = std::min(min_lst_theta, tasks[i].lst);
        }

        // TTEF: compute external occupation in [L, R)
        int interval_len = R - L;
        int total_occupied = count_set_bits(L, interval_len);
        int theta_cp_in_LR = 0;
        for (size_t i = 0; i <= k; ++i) {
            int clo = std::max(L, cp_lo_[tasks[i].idx]);
            int chi = std::min(R, cp_hi_[tasks[i].idx]);
            if (clo < chi) theta_cp_in_LR += chi - clo;
        }
        int external_occupied = total_occupied - theta_cp_in_LR;

        // Overload check (TTEF-enhanced)
        if (energy_theta + external_occupied > interval_len) return false;

        // Push tasks outside Theta
        for (size_t j = k + 1; j < m; ++j) {
            auto j_s_id = var_ids_[tasks[j].idx];
            if (direct ? vars_[tasks[j].idx]->is_assigned() : model.is_instantiated(j_s_id)) continue;

            int dur_j = tasks[j].dur;
            int lct_j = tasks[j].lct;

            // Subtract j's own CP from external_occupied
            int j_cp_in_LR = 0;
            {
                int clo = std::max(L, cp_lo_[tasks[j].idx]);
                int chi = std::min(R, cp_hi_[tasks[j].idx]);
                if (clo < chi) j_cp_in_LR = chi - clo;
            }
            int ext_for_j = external_occupied - j_cp_in_LR;

            // NOT-LAST condition (TTEF-enhanced)
            int effective_R = std::max(R, lct_j);
            if (L + energy_theta + dur_j + ext_for_j > effective_R) {
                int new_lst = R - energy_theta - ext_for_j - dur_j;
                // Simple DP: j must come before all Theta tasks
                new_lst = std::min(new_lst, min_lst_theta - dur_j);
                if (new_lst < tasks[j].lst) {
                    auto new_max = static_cast<Domain::value_type>(new_lst + offset_);
                    if (direct) {
                        if (!vars_[tasks[j].idx]->remove_above(new_max)) return false;
                    } else {
                        model.enqueue_set_max(j_s_id, new_max);
                    }
                }
            }
        }
    }

    // dur=0 forcing (non-strict): if timeline is fully packed, force dur to 0
    if (!strict_) {
        for (size_t i = 0; i < n_; ++i) {
            auto d_id = var_ids_[n_ + i];
            int dur_min_i = direct ? task_dur_min(i) : static_cast<int>(model.var_min(d_id));
            if (dur_min_i != 0) continue;
            if (direct ? vars_[n_ + i]->is_assigned() : model.is_instantiated(d_id)) continue;

            int est, lst;
            if (direct) {
                est = static_cast<int>(vars_[i]->min()) - offset_;
                lst = static_cast<int>(vars_[i]->max()) - offset_;
            } else {
                est = static_cast<int>(model.var_min(var_ids_[i])) - offset_;
                lst = static_cast<int>(model.var_max(var_ids_[i])) - offset_;
            }
            // Check if there's any free slot for dur=1 in [est, lst]
            int first_free = find_next_zero(est);
            bool can_fit = (first_free >= 0 && first_free <= lst && first_free + 1 <= horizon_);
            if (!can_fit) {
                if (direct) {
                    if (!vars_[n_ + i]->assign(0)) return false;
                } else {
                    model.enqueue_instantiate(d_id, 0);
                }
            }
        }
    }

    return true;
}

// ---------- Propagation ----------

bool DisjunctiveConstraint::propagate_bounds(Model& model) {
    for (size_t i = 0; i < n_; ++i) {
        auto s_id = var_ids_[i];
        auto d_id = var_ids_[n_ + i];
        if (model.is_instantiated(s_id)) continue;

        int dur;
        if (model.is_instantiated(d_id)) {
            dur = static_cast<int>(model.value(d_id));
        } else {
            dur = static_cast<int>(model.var_min(d_id));
        }

        if (dur == 0 && !strict_) {
            // dur_min=0: if no valid position for dur=1, force dur=0
            if (!model.is_instantiated(d_id) && vars_[n_ + i]->domain().size() > 1) {
                int lo = static_cast<int>(model.var_min(s_id)) - offset_;
                int hi = static_cast<int>(model.var_max(s_id)) - offset_;
                if (find_first_valid_excluding(lo, hi, 1, i) == -1) {
                    model.enqueue_instantiate(d_id, 0);
                }
            }
            continue;
        }
        if (dur <= 0) continue;

        int lo = static_cast<int>(model.var_min(s_id)) - offset_;  // est
        int hi = static_cast<int>(model.var_max(s_id)) - offset_;  // lst

        int new_lo = find_first_valid_excluding(lo, hi, dur, i);
        int new_hi = find_last_valid_excluding(lo, hi, dur, i);

        if (new_lo == -1 || new_hi == -1) return false;

        if (new_lo > lo) {
            model.enqueue_set_min(s_id,
                                  static_cast<Domain::value_type>(new_lo + offset_));
        }
        if (new_hi < hi) {
            model.enqueue_set_max(s_id,
                                  static_cast<Domain::value_type>(new_hi + offset_));
        }
    }

    // Edge-finding propagation
    if (!edge_finding(model, false)) return false;

    return true;
}

// ---------- Trail ----------

void DisjunctiveConstraint::rewind_to(int save_point) {
    // Rewind CP trail
    while (!cp_trail_.empty() && cp_trail_.back().first > save_point) {
        auto& entry = cp_trail_.back().second;
        cp_lo_[entry.task_idx] = entry.old_cp_lo;
        cp_hi_[entry.task_idx] = entry.old_cp_hi;
        cp_trail_.pop_back();
    }

    // Rewind timeline trail
    while (!trail_.empty() && trail_.back().first > save_point) {
        auto& entry = trail_.back().second;
        timeline_[entry.block_idx] = entry.old_mask;
        trail_.pop_back();
    }
}

// ---------- Presolve ----------

bool DisjunctiveConstraint::presolve(Model& model) {
    // Reset timeline and CP (presolve may be called multiple times in fixpoint loop)
    std::fill(timeline_.begin(), timeline_.end(), 0ULL);
    std::fill(cp_lo_.begin(), cp_lo_.end(), 0);
    std::fill(cp_hi_.begin(), cp_hi_.end(), 0);

    // Set bits for fully assigned tasks
    for (size_t i = 0; i < n_; ++i) {
        if (!task_fully_assigned(i)) continue;
        int d = task_dur(i);
        if (!strict_ && d == 0) continue;
        if (d <= 0) continue;
        int pos = task_start(i) - offset_;
        if (check_conflict(pos, d)) return false;
        set_bits_direct(pos, d);
        cp_lo_[i] = pos;
        cp_hi_[i] = pos + d;
    }

    // Set compulsory parts for unassigned tasks
    for (size_t i = 0; i < n_; ++i) {
        if (task_fully_assigned(i)) continue;
        if (!update_compulsory_part_direct(i)) return false;
    }

    // Bounds tightening for unassigned starts
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->is_assigned()) continue;

        int dur;
        if (vars_[n_ + i]->is_assigned()) {
            dur = task_dur(i);
        } else {
            dur = task_dur_min(i);
        }

        if (dur == 0 && !strict_) {
            if (!vars_[n_ + i]->is_assigned() && vars_[n_ + i]->domain().size() > 1) {
                int lo = static_cast<int>(vars_[i]->min()) - offset_;
                int hi = static_cast<int>(vars_[i]->max()) - offset_;
                if (find_first_valid_excluding(lo, hi, 1, i) == -1) {
                    if (!vars_[n_ + i]->assign(0)) return false;
                }
            }
            continue;
        }
        if (dur <= 0) continue;

        int lo = static_cast<int>(vars_[i]->min()) - offset_;
        int hi = static_cast<int>(vars_[i]->max()) - offset_;

        int new_lo = find_first_valid_excluding(lo, hi, dur, i);
        int new_hi = find_last_valid_excluding(lo, hi, dur, i);

        if (new_lo == -1 || new_hi == -1) return false;

        if (new_lo > lo) {
            if (!vars_[i]->remove_below(
                    static_cast<Domain::value_type>(new_lo + offset_)))
                return false;
        }
        if (new_hi < hi) {
            if (!vars_[i]->remove_above(
                    static_cast<Domain::value_type>(new_hi + offset_)))
                return false;
        }
    }

    // Edge-finding propagation
    if (!edge_finding(model, true)) return false;

    return true;
}

// ---------- Prepare propagation ----------

bool DisjunctiveConstraint::prepare_propagation(Model& model) {
    init_watches();

    // Reset timeline and CP
    std::fill(timeline_.begin(), timeline_.end(), 0ULL);
    trail_.clear();
    cp_trail_.clear();
    std::fill(cp_lo_.begin(), cp_lo_.end(), 0);
    std::fill(cp_hi_.begin(), cp_hi_.end(), 0);

    // Set bits for fully assigned tasks
    for (size_t i = 0; i < n_; ++i) {
        if (!task_fully_assigned(i)) continue;
        int d = task_dur(i);
        if (!strict_ && d == 0) continue;
        if (d <= 0) continue;
        int pos = task_start(i) - offset_;
        if (check_conflict(pos, d)) return false;
        set_bits_direct(pos, d);
        cp_lo_[i] = pos;
        cp_hi_[i] = pos + d;
    }

    // Set compulsory parts for unassigned tasks
    for (size_t i = 0; i < n_; ++i) {
        if (task_fully_assigned(i)) continue;
        if (!update_compulsory_part_direct(i)) return false;
    }

    return true;
}

// ---------- Event handlers ----------

bool DisjunctiveConstraint::on_instantiate(
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

    // Determine which task
    size_t task;
    if (internal_var_idx < n_) {
        task = internal_var_idx;       // start variable
    } else {
        task = internal_var_idx - n_;  // duration variable
    }

    // Update compulsory part for this task
    if (!update_compulsory_part(model, save_point, task)) return false;

    // Both start and duration must be assigned to place the task fully
    if (!(model.is_instantiated(var_ids_[task]) && model.is_instantiated(var_ids_[n_ + task]))) {
        if (!has_uninstantiated()) {
            return on_final_instantiate();
        }
        return propagate_bounds(model);
    }

    int s = static_cast<int>(model.value(var_ids_[task]));
    int d = static_cast<int>(model.value(var_ids_[n_ + task]));

    // Non-strict: zero-duration tasks don't occupy timeline
    if (!strict_ && d == 0) {
        if (!has_uninstantiated()) {
            return on_final_instantiate();
        }
        return propagate_bounds(model);
    }

    if (d <= 0) {
        if (!has_uninstantiated()) {
            return on_final_instantiate();
        }
        return propagate_bounds(model);
    }

    int pos = s - offset_;

    // Check conflict excluding own CP bits
    if (check_conflict_excluding(pos, d, task)) return false;

    // Set all bits for the fully assigned task
    set_bits(model, save_point, pos, d);

    // Update cp to cover full range
    if (cp_lo_[task] != pos || cp_hi_[task] != pos + d) {
        cp_trail_.push_back({save_point, {task, cp_lo_[task], cp_hi_[task]}});
        cp_lo_[task] = pos;
        cp_hi_[task] = pos + d;
    }

    if (!has_uninstantiated()) {
        return on_final_instantiate();
    }

    return propagate_bounds(model);
}

bool DisjunctiveConstraint::on_final_instantiate() {
    auto result = is_satisfied();
    return result.has_value() && result.value();
}

bool DisjunctiveConstraint::on_set_min(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/)
{
    size_t task = (internal_var_idx < n_) ? internal_var_idx : internal_var_idx - n_;
    if (!update_compulsory_part(model, save_point, task)) return false;
    return propagate_bounds(model);
}

bool DisjunctiveConstraint::on_set_max(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/)
{
    size_t task = (internal_var_idx < n_) ? internal_var_idx : internal_var_idx - n_;
    if (!update_compulsory_part(model, save_point, task)) return false;
    return propagate_bounds(model);
}

// ---------- Initial consistency ----------

void DisjunctiveConstraint::check_initial_consistency() {
    int64_t total_d_min = 0;
    int64_t global_est = 0;
    int64_t global_lct = 0;
    bool has_relevant = false;

    for (size_t i = 0; i < n_; ++i) {
        int64_t d_min = vars_[n_ + i]->min();
        if (!strict_ && d_min == 0) continue;
        if (d_min <= 0) continue;
        total_d_min += d_min;
        int64_t est_i = vars_[i]->min();
        int64_t lct_i = vars_[i]->max() + vars_[n_ + i]->max();
        if (!has_relevant) {
            global_est = est_i;
            global_lct = lct_i;
            has_relevant = true;
        } else {
            if (est_i < global_est) global_est = est_i;
            if (lct_i > global_lct) global_lct = lct_i;
        }
    }
    if (has_relevant && total_d_min > global_lct - global_est) {
        set_initially_inconsistent(true);
    }
}

}  // namespace sabori_csp
