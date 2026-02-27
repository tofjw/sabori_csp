/**
 * @file nogood_manager.cpp
 * @brief NoGood 管理クラスの実装
 */
#include "sabori_csp/nogood_manager.hpp"
#include "sabori_csp/solver.hpp"  // Literal, NoGood, NamedNoGood の定義

#include <algorithm>

namespace sabori_csp {

NoGoodManager::NoGoodManager() = default;

void NoGoodManager::clear() {
    nogoods_.clear();
    ng_eq_watches_.clear();
    ng_leq_watches_.clear();
    ng_geq_watches_.clear();
    unit_nogoods_.clear();
    ng_id_counter_ = 0;
    check_count_ = 0;
    prune_count_ = 0;
    domain_count_ = 0;
}

void NoGoodManager::clear(size_t n_vars) {
    clear();
    ng_eq_watches_.resize(n_vars);
    ng_leq_watches_.resize(n_vars);
    ng_geq_watches_.resize(n_vars);
}

// ===== Core operations =====

void NoGoodManager::add_nogood(const std::vector<Literal>& literals, size_t restart_count) {
    auto ng = std::make_unique<NoGood>(literals);
    auto* ng_ptr = ng.get();
    ng_ptr->id = ng_id_counter_++;
    ng_ptr->last_active = restart_count;

    register_watch(ng_ptr->literals[ng_ptr->w1], ng_ptr);
    if (ng_ptr->w1 != ng_ptr->w2) {
        register_watch(ng_ptr->literals[ng_ptr->w2], ng_ptr);
    }

    nogoods_.push_back(std::move(ng));
}

void NoGoodManager::remove_nogood(NoGood* ng) {
    unregister_watch(ng->literals[ng->w1], ng);
    if (ng->w1 != ng->w2) {
        unregister_watch(ng->literals[ng->w2], ng);
    }
}

// ===== Propagation =====

bool NoGoodManager::propagate_eq_watches(Model& model, size_t var_idx, Domain::value_type val,
                                          size_t restart_count, std::vector<double>& activity) {
    if (var_idx >= ng_eq_watches_.size()) return true;
    auto& var_watches = ng_eq_watches_[var_idx];
    auto it2 = var_watches.find(val);
    if (it2 == var_watches.end()) return true;

    // コピーして回す（propagate_nogood が watches を変更するため）
    for (auto* ng : std::vector<NoGood*>(it2->second)) {
        check_count_++;
        if (!propagate_nogood(model, ng, {var_idx, val, Literal::Type::Eq}, restart_count)) {
            ng->last_active = restart_count;
            prune_count_++;
            size_t n = ng->literals.size();
            for (const auto& lit : ng->literals) {
                activity[lit.var_idx] += 1.0 / n;
            }
            return false;
        }
    }

    return true;
}

bool NoGoodManager::propagate_bound_nogoods(Model& model, size_t var_idx, bool is_lower_bound,
                                             size_t restart_count, std::vector<double>& activity) {
    if (is_lower_bound) {
        // 下限が上がった → Geq リテラル (x >= v) が充足された可能性
        if (var_idx < ng_geq_watches_.size() && !ng_geq_watches_[var_idx].empty()) {
            auto current_min = model.var_min(var_idx);
            // コピーして回す（propagate_nogood が watches を変更するため）
            auto watches_copy = ng_geq_watches_[var_idx];
            for (const auto& [threshold, ng] : watches_copy) {
                if (current_min >= threshold) {
                    check_count_++;
                    if (!propagate_nogood(model, ng, {var_idx, threshold, Literal::Type::Geq}, restart_count)) {
                        ng->last_active = restart_count;
                        prune_count_++;
                        size_t n = ng->literals.size();
                        for (const auto& lit : ng->literals) {
                            activity[lit.var_idx] += 1.0 / n;
                        }
                        return false;
                    }
                }
            }
        }
    } else {
        // 上限が下がった → Leq リテラル (x <= v) が充足された可能性
        if (var_idx < ng_leq_watches_.size() && !ng_leq_watches_[var_idx].empty()) {
            auto current_max = model.var_max(var_idx);
            auto watches_copy = ng_leq_watches_[var_idx];
            for (const auto& [threshold, ng] : watches_copy) {
                if (current_max <= threshold) {
                    check_count_++;
                    if (!propagate_nogood(model, ng, {var_idx, threshold, Literal::Type::Leq}, restart_count)) {
                        ng->last_active = restart_count;
                        prune_count_++;
                        size_t n = ng->literals.size();
                        for (const auto& lit : ng->literals) {
                            activity[lit.var_idx] += 1.0 / n;
                        }
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool NoGoodManager::propagate_nogood(Model& model, NoGood* ng, const Literal& triggered,
                                      size_t restart_count) {
    auto& lits = ng->literals;

    // triggered が w1 か w2 か判定
    size_t triggered_idx, other_idx;
    if (lits[ng->w1] == triggered) {
        triggered_idx = ng->w1;
        other_idx = ng->w2;
    } else {
        triggered_idx = ng->w2;
        other_idx = ng->w1;
    }

    // 未成立リテラルを探す（watched 以外で）
    for (size_t i = 0; i < lits.size(); ++i) {
        if (i == ng->w1 || i == ng->w2) {
            continue;
        }
        if (!lits[i].is_satisfied(model)) {
            // 未成立 → watch をここに移す
            unregister_watch(triggered, ng);
            if (triggered_idx == ng->w1) {
                ng->w1 = i;
            } else {
                ng->w2 = i;
            }
            register_watch(lits[i], ng);
            return true;
        }
    }

    // 移せない → other の watched リテラルが唯一の未成立か確認
    const auto& other_lit = lits[other_idx];

    if (other_lit.is_satisfied(model)) {
        // 全リテラル成立 → 矛盾
        return false;
    }

    // Unit propagation: other_lit の否定を強制
    ng->last_active = restart_count;
    domain_count_++;
    switch (other_lit.type) {
    case Literal::Type::Eq:
        model.enqueue_remove_value(other_lit.var_idx, other_lit.value);
        break;
    case Literal::Type::Leq:
        // x <= v の否定 → x >= v+1
        model.enqueue_set_min(other_lit.var_idx, other_lit.value + 1);
        break;
    case Literal::Type::Geq:
        // x >= v の否定 → x <= v-1
        model.enqueue_set_max(other_lit.var_idx, other_lit.value - 1);
        break;
    }

    return true;
}

// ===== Unit NoGood =====

void NoGoodManager::add_unit_nogood(const Literal& lit) {
    unit_nogoods_.push_back(lit);
}

void NoGoodManager::enqueue_unit_nogoods(Model& model) const {
    for (const auto& lit : unit_nogoods_) {
        auto neg = lit.negate();
        switch (neg.type) {
        case Literal::Type::Eq:
            model.enqueue_remove_value(neg.var_idx, neg.value);
            break;
        case Literal::Type::Leq:
            model.enqueue_set_max(neg.var_idx, neg.value);
            break;
        case Literal::Type::Geq:
            model.enqueue_set_min(neg.var_idx, neg.value);
            break;
        }
    }
}

// ===== NoGood Learning =====

void NoGoodManager::learn_from_conflict(const std::vector<Literal>& decision_trail,
                                         std::vector<double>& activity, size_t restart_count) {
    if (decision_trail.size() >= 2) {
        add_nogood(decision_trail, restart_count);
        for (const auto& lit : decision_trail) {
            activity[lit.var_idx] += 0.01 / decision_trail.size();
        }
    } else if (decision_trail.size() == 1) {
        unit_nogoods_.push_back(decision_trail[0]);
    }
}

// ===== Solution NoGood =====

void NoGoodManager::add_solution_nogood(const Model& model, size_t restart_count) {
    std::vector<Literal> lits;
    const auto& variables = model.variables();
    for (size_t i = 0; i < variables.size(); ++i) {
        // 定数変数（initial_range == 1）を除外: 全解で同じ値なので情報がなく、
        // NoGood の watched literal が定数に設定されると機能しなくなる
        if (model.is_instantiated(i) && model.initial_range(i) > 1) {
            lits.push_back({i, model.value(i)});
        }
    }
    if (!lits.empty()) {
        add_nogood(lits, restart_count);
        nogoods_.back()->permanent = true;
    }
}

// ===== Maintenance (GC) =====

void NoGoodManager::gc(size_t restart_count, size_t inactive_limit) {
    // 非活性 NoGood の削除
    nogoods_.erase(
        std::remove_if(nogoods_.begin(), nogoods_.end(),
            [&](const auto& ng) {
                if (ng->permanent) return false;
                if (restart_count - ng->last_active >= inactive_limit) {
                    remove_nogood(ng.get());
                    return true;
                }
                return false;
            }),
        nogoods_.end());

    // 有用度順にソート: permanent を先頭、次に last_active が大きいものを優先
    std::stable_sort(nogoods_.begin(), nogoods_.end(),
        [](const auto& a, const auto& b) {
            if (a->permanent != b->permanent) return a->permanent > b->permanent;
            return a->last_active > b->last_active;
        });

    // 容量管理: 末尾（冷たい NG）を削除（permanent は保護）
    while (nogoods_.size() > max_nogoods_) {
        if (nogoods_.back()->permanent) break;
        remove_nogood(nogoods_.back().get());
        nogoods_.pop_back();
    }
}

// ===== Bloom Filter =====

Bloom512 NoGoodManager::ng_bloom_bits(size_t ng_id) {
    constexpr uint64_t k1 = 11400714819323198485ULL;
    constexpr uint64_t k2 = 7046029254386353131ULL;
    Bloom512 b;
    uint64_t h1 = ng_id * k1;
    uint64_t h2 = ng_id * k2;
    unsigned pos1 = h1 >> 55;  // 0..511
    unsigned pos2 = h2 >> 55;
    b.w[pos1 / 64] |= 1ULL << (pos1 % 64);
    b.w[pos2 / 64] |= 1ULL << (pos2 % 64);
    return b;
}

void NoGoodManager::rebuild_var_ng_blooms(Model& model) const {
    model.clear_var_ng_blooms();
    for (const auto& ng : nogoods_) {
        auto bits = ng_bloom_bits(ng->id);
        for (const auto& lit : ng->literals) {
            model.or_var_ng_bloom(lit.var_idx, bits);
        }
    }
}

// ===== Backtrack Support =====

void NoGoodManager::truncate_nogoods(size_t count) {
    while (nogoods_.size() > count) {
        remove_nogood(nogoods_.back().get());
        nogoods_.pop_back();
    }
}

// ===== Import / Export =====

std::vector<NamedNoGood> NoGoodManager::get_nogoods(const Model& model, size_t max_count) const {
    std::vector<NamedNoGood> result;
    size_t count = 0;

    for (const auto& ng : nogoods_) {
        if (max_count > 0 && count >= max_count) {
            break;
        }

        NamedNoGood named_ng;
        bool valid = true;

        for (const auto& lit : ng->literals) {
            if (lit.var_idx >= model.variables().size()) {
                valid = false;
                break;
            }
            auto var = model.variable(lit.var_idx);
            if (!var) {
                valid = false;
                break;
            }
            named_ng.literals.push_back({var->name(), lit.value, lit.type});
        }

        if (valid && !named_ng.literals.empty()) {
            result.push_back(std::move(named_ng));
            ++count;
        }
    }

    return result;
}

size_t NoGoodManager::add_nogoods(const std::vector<NamedNoGood>& nogoods,
                                   const Model& model, size_t restart_count) {
    // 変数名 → インデックスのマップを構築
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < model.variables().size(); ++i) {
        auto var = model.variable(i);
        if (var) {
            name_to_idx[var->name()] = i;
        }
    }

    size_t added = 0;
    for (const auto& named_ng : nogoods) {
        std::vector<Literal> literals;
        bool valid = true;

        for (const auto& named_lit : named_ng.literals) {
            auto it = name_to_idx.find(named_lit.var_name);
            if (it == name_to_idx.end()) {
                valid = false;
                break;
            }
            literals.push_back({it->second, named_lit.value, named_lit.type});
        }

        if (valid && !literals.empty()) {
            add_nogood(literals, restart_count);
            ++added;
        }
    }

    return added;
}

// ===== Debug =====

std::map<size_t, size_t> NoGoodManager::length_distribution() const {
    std::map<size_t, size_t> dist;
    if (!unit_nogoods_.empty()) dist[1] += unit_nogoods_.size();
    for (const auto& ng : nogoods_) {
        dist[ng->literals.size()]++;
    }
    return dist;
}

// ===== Watch helpers =====

void NoGoodManager::register_watch(const Literal& lit, NoGood* ng) {
    switch (lit.type) {
    case Literal::Type::Eq:
        ng_eq_watches_[lit.var_idx][lit.value].push_back(ng);
        break;
    case Literal::Type::Leq:
        ng_leq_watches_[lit.var_idx].emplace_back(lit.value, ng);
        break;
    case Literal::Type::Geq:
        ng_geq_watches_[lit.var_idx].emplace_back(lit.value, ng);
        break;
    }
}

void NoGoodManager::unregister_watch(const Literal& lit, NoGood* ng) {
    switch (lit.type) {
    case Literal::Type::Eq: {
        auto& watches = ng_eq_watches_[lit.var_idx][lit.value];
        watches.erase(std::remove(watches.begin(), watches.end(), ng), watches.end());
        break;
    }
    case Literal::Type::Leq: {
        auto& watches = ng_leq_watches_[lit.var_idx];
        watches.erase(
            std::remove_if(watches.begin(), watches.end(),
                [&](const auto& p) { return p.first == lit.value && p.second == ng; }),
            watches.end());
        break;
    }
    case Literal::Type::Geq: {
        auto& watches = ng_geq_watches_[lit.var_idx];
        watches.erase(
            std::remove_if(watches.begin(), watches.end(),
                [&](const auto& p) { return p.first == lit.value && p.second == ng; }),
            watches.end());
        break;
    }
    }
}

} // namespace sabori_csp
