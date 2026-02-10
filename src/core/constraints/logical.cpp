#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// ArrayBoolAndConstraint implementation
// ============================================================================

ArrayBoolAndConstraint::ArrayBoolAndConstraint(std::vector<VariablePtr> vars, VariablePtr r)
    : Constraint([&]() {
        std::vector<VariablePtr> all_vars = vars;
        all_vars.push_back(r);
        return all_vars;
    }())
    , vars_(std::move(vars))
    , r_(std::move(r))
    , n_(vars_.size())
    , w1_(0)
    , w2_(n_ > 1 ? 1 : 0) {

    // 変数ポインタ/ID → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
        var_id_to_idx_[vars_[i]->id()] = i;
    }
    var_ptr_to_idx_[r_.get()] = n_;  // r は index n
    var_id_to_idx_[r_->id()] = n_;

    // 初期 watch を設定: 0 になりうる（= 未確定 or 0 を含む）変数を探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        if (!vars_[i]->is_assigned() || vars_[i]->assigned_value().value() == 0) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }
    // watch が2つ見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX) w1_ = 0;
    if (w2_ == SIZE_MAX) w2_ = (n_ > 1) ? 1 : 0;

    // 注意: 内部状態は presolve() で初期化
}

std::string ArrayBoolAndConstraint::name() const {
    return "array_bool_and";
}

std::vector<VariablePtr> ArrayBoolAndConstraint::variables() const {
    std::vector<VariablePtr> result = vars_;
    result.push_back(r_);
    return result;
}

std::optional<bool> ArrayBoolAndConstraint::is_satisfied() const {
    // 全変数が確定しているか
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
    }
    if (!r_->is_assigned()) {
        return std::nullopt;
    }

    // 全ての bi の AND を計算
    bool and_result = true;
    for (const auto& var : vars_) {
        if (var->assigned_value().value() == 0) {
            and_result = false;
            break;
        }
    }

    return and_result == (r_->assigned_value().value() == 1);
}

bool ArrayBoolAndConstraint::prepare_propagation(Model& model) {
    // watch を再初期化: 0 になりうる変数を探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        if (!vars_[i]->is_assigned() || vars_[i]->assigned_value().value() == 0) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }
    // watch が2つ見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX) w1_ = 0;
    if (w2_ == SIZE_MAX) w2_ = (n_ > 1) ? 1 : 0;

    // 2WL を初期化
    init_watches();


    // 初期整合性チェック
    // r = 1 だが bi = 0 が存在する場合は矛盾
    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        for (const auto& var : vars_) {
            if (var->is_assigned() && var->assigned_value().value() == 0) {
                return false;
            }
        }
    }

    // r = 0 だが全ての bi = 1 の場合は矛盾
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        bool all_one = true;
        for (const auto& var : vars_) {
            if (!var->is_assigned() || var->assigned_value().value() != 1) {
                all_one = false;
                break;
            }
        }
        if (all_one) {
            return false;
        }
    }

    return true;
}

bool ArrayBoolAndConstraint::presolve(Model& model) {
    // 1. bi の中に 0 が確定しているものがあれば r = 0
    for (const auto& var : vars_) {
        if (var->is_assigned() && var->assigned_value().value() == 0) {
            if (r_->is_assigned()) {
                return r_->assigned_value().value() == 0;
            }
            if (!r_->domain().contains(0)) {
                return false;
            }
            r_->assign(0);
            return true;
        }
    }

    // 2. 全ての bi = 1 が確定していれば r = 1
    bool all_one = true;
    for (const auto& var : vars_) {
        if (!var->is_assigned() || var->assigned_value().value() != 1) {
            all_one = false;
            break;
        }
    }
    if (all_one) {
        if (r_->is_assigned()) {
            return r_->assigned_value().value() == 1;
        }
        if (!r_->domain().contains(1)) {
            return false;
        }
        r_->assign(1);
        return true;
    }

    // 3. r = 1 が確定していれば全ての bi = 1
    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        for (const auto& var : vars_) {
            if (!var->is_assigned()) {
                if (!var->domain().contains(1)) {
                    return false;
                }
                var->assign(1);
            } else if (var->assigned_value().value() != 1) {
                return false;
            }
        }
    }

    // 4. r = 0 が確定していて、未確定の bi が1つだけなら、その bi = 0
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        size_t unassigned_count = 0;
        size_t last_unassigned = SIZE_MAX;
        bool has_zero = false;

        for (size_t i = 0; i < n_; ++i) {
            if (vars_[i]->is_assigned()) {
                if (vars_[i]->assigned_value().value() == 0) {
                    has_zero = true;
                    break;
                }
            } else {
                unassigned_count++;
                last_unassigned = i;
            }
        }

        if (!has_zero && unassigned_count == 1) {
            // 最後の未確定変数を 0 に
            if (!vars_[last_unassigned]->domain().contains(0)) {
                return false;
            }
            vars_[last_unassigned]->assign(0);
        }
    }

    return true;
}

bool ArrayBoolAndConstraint::on_instantiate(Model& model, int save_point,
                                             size_t var_idx, Domain::value_type value,
                                             Domain::value_type prev_min,
                                             Domain::value_type prev_max) {
    // 基底クラスの処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) で内部インデックスを取得
    auto it = var_id_to_idx_.find(var_idx);
    if (it == var_id_to_idx_.end()) {
        return true;  // この制約に関係ない変数
    }
    size_t internal_idx = it->second;

    // r が確定した場合
    if (internal_idx == n_) {
        if (value == 1) {
            // r = 1: 全ての bi = 1（キューイング）
            for (size_t i = 0; i < n_; ++i) {
                if (!vars_[i]->is_assigned()) {
                    model.enqueue_instantiate(vars_[i]->id(), 1);
                } else if (vars_[i]->assigned_value().value() != 1) {
                    return false;  // 既に 0 が確定している bi がある
                }
            }
            return true;
        }
        // r = 0: 0 になりうる bi をスキャンし、watch を再初期化
        size_t candidate_count = 0;
        size_t first_candidate = SIZE_MAX;
        size_t second_candidate = SIZE_MAX;
        size_t last_unassigned = SIZE_MAX;
        size_t unassigned_count = 0;

        for (size_t i = 0; i < n_; ++i) {
            if (!vars_[i]->is_assigned()) {
                candidate_count++;
                unassigned_count++;
                last_unassigned = i;
                if (first_candidate == SIZE_MAX) first_candidate = i;
                else if (second_candidate == SIZE_MAX) second_candidate = i;
            } else if (vars_[i]->assigned_value().value() == 0) {
                // 既に 0 の bi がある → r = 0 は既に充足
                return true;
            }
        }

        if (candidate_count == 0) {
            // 全 bi = 1 なので AND=1 ≠ r=0 → 矛盾
            return false;
        }

        if (unassigned_count == 1) {
            // 未確定が1つだけ → それを 0 に強制
            model.enqueue_instantiate(vars_[last_unassigned]->id(), 0);
        }

        // watch を有効な候補に更新
        w1_ = first_candidate;
        w2_ = (second_candidate != SIZE_MAX) ? second_candidate : first_candidate;
        return true;
    }

    // bi が確定した場合
    if (value == 0) {
        // bi = 0 → r = 0（キューイング）
        if (!r_->is_assigned()) {
            model.enqueue_instantiate(r_->id(), 0);
        } else if (r_->assigned_value().value() != 0) {
            return false;  // r = 1 だが bi = 0
        }
        return true;
    }

    // bi = 1 が確定した場合
    // 全ての bi = 1 なら r = 1
    bool all_one = true;
    for (size_t i = 0; i < n_; ++i) {
        if (!vars_[i]->is_assigned()) {
            all_one = false;
            break;
        } else if (vars_[i]->assigned_value().value() != 1) {
            all_one = false;
            break;
        }
    }
    if (all_one) {
        if (!r_->is_assigned()) {
            model.enqueue_instantiate(r_->id(), 1);
        } else if (r_->assigned_value().value() != 1) {
            return false;  // 全 bi=1 だが r=0 → 矛盾
        }
    }

    // r = 0 で bi = 1 が確定した場合: 2WL 処理
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        // この bi が watched だった場合、別の候補に移す
        if (internal_idx == w1_ || internal_idx == w2_) {
            size_t watched_idx = (internal_idx == w1_) ? 1 : 2;
            size_t other_watch = (internal_idx == w1_) ? w2_ : w1_;

            // 新しい watch 候補を探す
            size_t new_watch = find_unwatched_candidate(w1_, w2_);

            if (new_watch != SIZE_MAX) {
                // 移動可能
                move_watch(model, save_point, watched_idx, new_watch);
            } else {
                // 移動先がない: もう一方の watched 変数をチェック
                if (vars_[other_watch]->is_assigned()) {
                    if (vars_[other_watch]->assigned_value().value() == 1) {
                        // 両方の watch が 1 に確定 → 0 になる変数がない → 矛盾
                        // （r = 0 を満たすには少なくとも1つの bi = 0 が必要）
                        return false;
                    }
                    // other_watch = 0 なら OK（r = 0 が満たされる）
                } else {
                    // other_watch が未確定 → それを 0 に確定（キューイング）
                    model.enqueue_instantiate(vars_[other_watch]->id(), 0);
                }
            }
        }
    }

    return true;
}

bool ArrayBoolAndConstraint::on_final_instantiate() {
    bool and_result = true;
    for (const auto& var : vars_) {
        if (var->assigned_value().value() == 0) {
            and_result = false;
            break;
        }
    }
    return and_result == (r_->assigned_value().value() == 1);
}

bool ArrayBoolAndConstraint::on_last_uninstantiated(Model& model, int save_point,
                                                     size_t last_var_internal_idx) {
    (void)save_point;

    if (last_var_internal_idx == n_) {
        // r が最後の未確定変数
        bool all_one = true;
        for (const auto& var : vars_) {
            if (var->assigned_value().value() == 0) {
                all_one = false;
                break;
            }
        }
        model.enqueue_instantiate(r_->id(), all_one ? 1 : 0);
    } else {
        // bi が最後の未確定変数
        if (r_->is_assigned()) {
            if (r_->assigned_value().value() == 1) {
                // r = 1 なら bi = 1
                model.enqueue_instantiate(vars_[last_var_internal_idx]->id(), 1);
            } else {
                // r = 0 で他の全ての bj = 1 なら bi = 0
                bool others_all_one = true;
                for (size_t i = 0; i < n_; ++i) {
                    if (i != last_var_internal_idx) {
                        if (vars_[i]->assigned_value().value() == 0) {
                            others_all_one = false;
                            break;
                        }
                    }
                }
                if (others_all_one) {
                    model.enqueue_instantiate(vars_[last_var_internal_idx]->id(), 0);
                }
            }
        }
    }

    return true;
}

void ArrayBoolAndConstraint::check_initial_consistency() {
    // r = 1 だが bi = 0 が存在する場合は矛盾
    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        for (const auto& var : vars_) {
            if (var->is_assigned() && var->assigned_value().value() == 0) {
                set_initially_inconsistent(true);
                return;
            }
        }
    }

    // r = 0 だが全ての bi = 1 の場合は矛盾
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        bool all_one = true;
        for (const auto& var : vars_) {
            if (!var->is_assigned() || var->assigned_value().value() != 1) {
                all_one = false;
                break;
            }
        }
        if (all_one) {
            set_initially_inconsistent(true);
            return;
        }
    }

    // bi = 0 が存在するが r = 1 が強制されている場合
    bool has_zero = false;
    for (const auto& var : vars_) {
        if (var->is_assigned() && var->assigned_value().value() == 0) {
            has_zero = true;
            break;
        }
    }
    if (has_zero && r_->is_assigned() && r_->assigned_value().value() == 1) {
        set_initially_inconsistent(true);
    }
}

size_t ArrayBoolAndConstraint::find_unwatched_candidate(size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        // 0 になりうる（未確定 or 0 を含むドメイン）
        if (!vars_[i]->is_assigned()) {
            return i;
        }
        if (vars_[i]->assigned_value().value() == 0) {
            return i;  // 既に 0 に確定している
        }
    }
    return SIZE_MAX;
}

void ArrayBoolAndConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// ArrayBoolOrConstraint implementation
// ============================================================================

ArrayBoolOrConstraint::ArrayBoolOrConstraint(std::vector<VariablePtr> vars, VariablePtr r)
    : Constraint([&]() {
        std::vector<VariablePtr> all_vars = vars;
        all_vars.push_back(r);
        return all_vars;
    }())
    , vars_(std::move(vars))
    , r_(std::move(r))
    , n_(vars_.size())
    , w1_(0)
    , w2_(n_ > 1 ? 1 : 0) {

    // 変数ポインタ/ID → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
        var_id_to_idx_[vars_[i]->id()] = i;
    }
    var_ptr_to_idx_[r_.get()] = n_;
    var_id_to_idx_[r_->id()] = n_;

    // 初期 watch: 1 になりうる変数を探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        if (!vars_[i]->is_assigned() || vars_[i]->assigned_value().value() == 1) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }
    if (w1_ == SIZE_MAX) w1_ = 0;
    if (w2_ == SIZE_MAX) w2_ = (n_ > 1) ? 1 : 0;

    check_initial_consistency();
}

std::string ArrayBoolOrConstraint::name() const {
    return "array_bool_or";
}

std::vector<VariablePtr> ArrayBoolOrConstraint::variables() const {
    std::vector<VariablePtr> result = vars_;
    result.push_back(r_);
    return result;
}

std::optional<bool> ArrayBoolOrConstraint::is_satisfied() const {
    for (const auto& var : vars_) {
        if (!var->is_assigned()) return std::nullopt;
    }
    if (!r_->is_assigned()) return std::nullopt;

    bool or_result = false;
    for (const auto& var : vars_) {
        if (var->assigned_value().value() == 1) {
            or_result = true;
            break;
        }
    }
    return or_result == (r_->assigned_value().value() == 1);
}

bool ArrayBoolOrConstraint::presolve(Model& model) {
    // 1. bi の中に 1 が確定しているものがあれば r = 1
    for (const auto& var : vars_) {
        if (var->is_assigned() && var->assigned_value().value() == 1) {
            if (r_->is_assigned()) {
                return r_->assigned_value().value() == 1;
            }
            if (!r_->domain().contains(1)) return false;
            r_->assign(1);
            return true;
        }
    }

    // 2. 全ての bi = 0 が確定していれば r = 0
    bool all_zero = true;
    for (const auto& var : vars_) {
        if (!var->is_assigned() || var->assigned_value().value() != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        if (r_->is_assigned()) {
            return r_->assigned_value().value() == 0;
        }
        if (!r_->domain().contains(0)) return false;
        r_->assign(0);
        return true;
    }

    // 3. r = 0 なら全ての bi = 0
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        for (const auto& var : vars_) {
            if (!var->is_assigned()) {
                if (!var->domain().contains(0)) return false;
                var->assign(0);
            } else if (var->assigned_value().value() != 0) {
                return false;
            }
        }
    }

    // 4. r = 1 で未確定の bi が1つだけなら、その bi = 1
    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        size_t unassigned_count = 0;
        size_t last_unassigned = SIZE_MAX;
        bool has_one = false;

        for (size_t i = 0; i < n_; ++i) {
            if (vars_[i]->is_assigned()) {
                if (vars_[i]->assigned_value().value() == 1) {
                    has_one = true;
                    break;
                }
            } else {
                unassigned_count++;
                last_unassigned = i;
            }
        }

        if (!has_one && unassigned_count == 1) {
            if (!vars_[last_unassigned]->domain().contains(1)) return false;
            vars_[last_unassigned]->assign(1);
        }
    }

    return true;
}

bool ArrayBoolOrConstraint::on_instantiate(Model& model, int save_point,
                                            size_t var_idx, Domain::value_type value,
                                            Domain::value_type prev_min,
                                            Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) で内部インデックスを取得
    auto it = var_id_to_idx_.find(var_idx);
    if (it == var_id_to_idx_.end()) return true;
    size_t internal_idx = it->second;

    // r が確定した場合
    if (internal_idx == n_) {
        if (value == 0) {
            // r = 0: 全ての bi = 0（キューイング）
            for (size_t i = 0; i < n_; ++i) {
                if (!vars_[i]->is_assigned()) {
                    model.enqueue_instantiate(vars_[i]->id(), 0);
                } else if (vars_[i]->assigned_value().value() != 0) {
                    return false;
                }
            }
        } else {
            // r = 1: 1 になりうる bi をスキャンし、watch を再初期化
            size_t candidate_count = 0;
            size_t first_candidate = SIZE_MAX;
            size_t second_candidate = SIZE_MAX;
            size_t last_unassigned = SIZE_MAX;
            size_t unassigned_count = 0;

            for (size_t i = 0; i < n_; ++i) {
                if (!vars_[i]->is_assigned()) {
                    candidate_count++;
                    unassigned_count++;
                    last_unassigned = i;
                    if (first_candidate == SIZE_MAX) first_candidate = i;
                    else if (second_candidate == SIZE_MAX) second_candidate = i;
                } else if (vars_[i]->assigned_value().value() == 1) {
                    // 既に 1 の bi がある → r = 1 は既に充足
                    return true;
                }
            }

            if (candidate_count == 0) {
                // 全 bi = 0 なので OR=0 ≠ r=1 → 矛盾
                return false;
            }

            if (unassigned_count == 1) {
                // 未確定が1つだけ → それを 1 に強制
                model.enqueue_instantiate(vars_[last_unassigned]->id(), 1);
            }

            // watch を有効な候補に更新
            w1_ = first_candidate;
            w2_ = (second_candidate != SIZE_MAX) ? second_candidate : first_candidate;
        }
        return true;
    }

    // bi が確定した場合
    if (value == 1) {
        // bi = 1 → r = 1（キューイング）
        if (!r_->is_assigned()) {
            model.enqueue_instantiate(r_->id(), 1);
        } else if (r_->assigned_value().value() != 1) {
            return false;
        }
        return true;
    }

    // bi = 0 が確定した場合
    // 全ての bi = 0 なら r = 0
    bool all_zero = true;
    for (size_t i = 0; i < n_; ++i) {
        if (!vars_[i]->is_assigned()) {
            all_zero = false;
            break;
        } else if (vars_[i]->assigned_value().value() != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        if (!r_->is_assigned()) {
            model.enqueue_instantiate(r_->id(), 0);
        } else if (r_->assigned_value().value() != 0) {
            return false;  // 全 bi=0 だが r=1 → 矛盾
        }
    }

    // r = 1 で bi = 0 が確定した場合: 2WL 処理
    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        if (internal_idx == w1_ || internal_idx == w2_) {
            size_t watched_idx = (internal_idx == w1_) ? 1 : 2;
            size_t other_watch = (internal_idx == w1_) ? w2_ : w1_;

            size_t new_watch = find_unwatched_candidate(w1_, w2_);

            if (new_watch != SIZE_MAX) {
                move_watch(model, save_point, watched_idx, new_watch);
            } else {
                if (vars_[other_watch]->is_assigned()) {
                    if (vars_[other_watch]->assigned_value().value() == 0) {
                        return false;  // 全て 0 になってしまう → r = 1 が満たせない
                    }
                } else {
                    // other_watch が未確定 → それを 1 に確定（キューイング）
                    model.enqueue_instantiate(vars_[other_watch]->id(), 1);
                }
            }
        }
    }

    return true;
}

bool ArrayBoolOrConstraint::on_final_instantiate() {
    bool or_result = false;
    for (const auto& var : vars_) {
        if (var->assigned_value().value() == 1) {
            or_result = true;
            break;
        }
    }
    return or_result == (r_->assigned_value().value() == 1);
}

bool ArrayBoolOrConstraint::on_last_uninstantiated(Model& model, int save_point,
                                                    size_t last_var_internal_idx) {
    (void)save_point;

    if (last_var_internal_idx == n_) {
        bool has_one = false;
        for (const auto& var : vars_) {
            if (var->assigned_value().value() == 1) {
                has_one = true;
                break;
            }
        }
        model.enqueue_instantiate(r_->id(), has_one ? 1 : 0);
    } else {
        if (r_->is_assigned()) {
            if (r_->assigned_value().value() == 0) {
                model.enqueue_instantiate(vars_[last_var_internal_idx]->id(), 0);
            } else {
                bool others_have_one = false;
                for (size_t i = 0; i < n_; ++i) {
                    if (i != last_var_internal_idx) {
                        if (vars_[i]->assigned_value().value() == 1) {
                            others_have_one = true;
                            break;
                        }
                    }
                }
                if (!others_have_one) {
                    model.enqueue_instantiate(vars_[last_var_internal_idx]->id(), 1);
                }
            }
        }
    }

    return true;
}

void ArrayBoolOrConstraint::check_initial_consistency() {
    if (r_->is_assigned() && r_->assigned_value().value() == 0) {
        for (const auto& var : vars_) {
            if (var->is_assigned() && var->assigned_value().value() == 1) {
                set_initially_inconsistent(true);
                return;
            }
        }
    }

    if (r_->is_assigned() && r_->assigned_value().value() == 1) {
        bool all_zero = true;
        for (const auto& var : vars_) {
            if (!var->is_assigned() || var->assigned_value().value() != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            set_initially_inconsistent(true);
            return;
        }
    }

    bool has_one = false;
    for (const auto& var : vars_) {
        if (var->is_assigned() && var->assigned_value().value() == 1) {
            has_one = true;
            break;
        }
    }
    if (has_one && r_->is_assigned() && r_->assigned_value().value() == 0) {
        set_initially_inconsistent(true);
    }
}

size_t ArrayBoolOrConstraint::find_unwatched_candidate(size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        if (!vars_[i]->is_assigned()) {
            return i;
        }
        if (vars_[i]->assigned_value().value() == 1) {
            return i;
        }
    }
    return SIZE_MAX;
}

void ArrayBoolOrConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// BoolClauseConstraint implementation
// ============================================================================

BoolClauseConstraint::BoolClauseConstraint(std::vector<VariablePtr> pos, std::vector<VariablePtr> neg)
    : Constraint([&]() {
        std::vector<VariablePtr> all_vars = pos;
        all_vars.insert(all_vars.end(), neg.begin(), neg.end());
        return all_vars;
    }())
    , pos_(std::move(pos))
    , neg_(std::move(neg))
    , n_pos_(pos_.size())
    , n_neg_(neg_.size())
    , w1_(SIZE_MAX)
    , w2_(SIZE_MAX) {

    // 変数ポインタ → リテラルインデックスマップを構築
    // 変数ID → リテラルインデックスマップを構築
    // 0 <= idx < n_pos_: pos_[idx]
    // n_pos_ <= idx < n_pos_ + n_neg_: neg_[idx - n_pos_]
    for (size_t i = 0; i < n_pos_; ++i) {
        var_ptr_to_idx_[pos_[i].get()] = i;
        var_id_to_lit_idx_[pos_[i]->id()] = i;
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        // neg の変数が pos にも含まれている場合は上書きしない
        // （同じ変数が両方に含まれるケースは稀だが対応）
        if (var_ptr_to_idx_.find(neg_[i].get()) == var_ptr_to_idx_.end()) {
            var_ptr_to_idx_[neg_[i].get()] = n_pos_ + i;
            var_id_to_lit_idx_[neg_[i]->id()] = n_pos_ + i;
        }
    }

    // 初期 watch を設定: 節を充足しうるリテラルを2つ探す
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(i)) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }

    // watch が見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX && n_pos_ + n_neg_ > 0) w1_ = 0;
    if (w2_ == SIZE_MAX && n_pos_ + n_neg_ > 1) w2_ = 1;
    if (w2_ == SIZE_MAX) w2_ = w1_;

    // 注意: 内部状態は presolve() で初期化
}

std::string BoolClauseConstraint::name() const {
    return "bool_clause";
}

std::vector<VariablePtr> BoolClauseConstraint::variables() const {
    std::vector<VariablePtr> result = pos_;
    result.insert(result.end(), neg_.begin(), neg_.end());
    return result;
}

std::optional<bool> BoolClauseConstraint::is_satisfied() const {
    // 充足しているリテラルがあるか
    for (size_t i = 0; i < n_pos_; ++i) {
        if (!pos_[i]->is_assigned()) return std::nullopt;
        if (pos_[i]->assigned_value().value() == 1) return true;
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        if (!neg_[i]->is_assigned()) return std::nullopt;
        if (neg_[i]->assigned_value().value() == 0) return true;
    }
    // 全リテラルが確定し、いずれも充足していない
    return false;
}

bool BoolClauseConstraint::prepare_propagation(Model& model) {
    // watch を再初期化: 節を充足しうるリテラルを2つ探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(i)) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }

    // watch が見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX && n_pos_ + n_neg_ > 0) w1_ = 0;
    if (w2_ == SIZE_MAX && n_pos_ + n_neg_ > 1) w2_ = 1;
    if (w2_ == SIZE_MAX) w2_ = w1_;

    // 2WL を初期化
    init_watches();


    // 初期整合性チェック: 全てのリテラルが充足不可能なら矛盾
    bool has_satisfiable = false;
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(i)) {
            has_satisfiable = true;
            break;
        }
    }
    if (!has_satisfiable) {
        return false;
    }

    return true;
}

bool BoolClauseConstraint::presolve(Model& model) {
    // 既に充足しているかチェック
    for (size_t i = 0; i < n_pos_; ++i) {
        if (pos_[i]->is_assigned() && pos_[i]->assigned_value().value() == 1) {
            return true;  // 充足
        }
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        if (neg_[i]->is_assigned() && neg_[i]->assigned_value().value() == 0) {
            return true;  // 充足
        }
    }

    // 充足可能なリテラルをカウント
    size_t satisfiable_count = 0;
    size_t last_satisfiable = SIZE_MAX;

    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(i)) {
            satisfiable_count++;
            last_satisfiable = i;
        }
    }

    if (satisfiable_count == 0) {
        return false;  // 矛盾
    }

    if (satisfiable_count == 1) {
        // Unit propagation: 唯一の充足可能リテラルを確定
        VariablePtr var = get_var(last_satisfiable);
        Domain::value_type val = satisfying_value(last_satisfiable);
        if (!var->is_assigned()) {
            if (!var->domain().contains(val)) return false;
            var->assign(val);
        }
    }

      
    return true;
}

bool BoolClauseConstraint::on_instantiate(Model& model, int save_point,
                                           size_t var_idx, Domain::value_type value,
                                           Domain::value_type prev_min,
                                           Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) でリテラルインデックスを取得
    auto it = var_id_to_lit_idx_.find(var_idx);
    if (it == var_id_to_lit_idx_.end()) {
        return true;  // この制約に関係ない変数
    }
    size_t assigned_lit = it->second;

    // このリテラルが節を充足したか（O(1)）
    if (is_satisfied_by(assigned_lit)) {
        return true;
    }

    // watch が充足していれば節は充足（O(1)）
    if (is_satisfied_by(w1_) || is_satisfied_by(w2_)) {
        return true;
    }

    // このリテラルが watched だった場合、別の候補に移す
    if (assigned_lit == w1_ || assigned_lit == w2_) {
        size_t watched_idx = (assigned_lit == w1_) ? 1 : 2;
        size_t other_watch = (assigned_lit == w1_) ? w2_ : w1_;

        // 新しい watch 候補を探す
        size_t new_watch = find_unwatched_candidate(w1_, w2_);

        if (new_watch != SIZE_MAX) {
            move_watch(model, save_point, watched_idx, new_watch);
        } else {
            // 移動先がない
            if (!can_satisfy(other_watch)) {
                // もう一方も充足不可能 → 矛盾
                return false;
            }

            VariablePtr other_var = get_var(other_watch);
            if (other_var->is_assigned()) {
                // 既に確定している場合
                if (!is_satisfied_by(other_watch)) {
                    return false;  // 充足していない → 矛盾
                }
            } else {
                // Unit propagation: other_watch を充足方向に確定
                model.enqueue_instantiate(other_var->id(), satisfying_value(other_watch));
            }
        }
    }

    return true;
}

bool BoolClauseConstraint::on_final_instantiate() {
    // いずれかのリテラルが充足しているか
    for (size_t i = 0; i < n_pos_; ++i) {
        if (pos_[i]->is_assigned() && pos_[i]->assigned_value().value() == 1) return true;
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        if (neg_[i]->is_assigned() && neg_[i]->assigned_value().value() == 0) return true;
    }
    return false;
}

bool BoolClauseConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                    size_t last_var_internal_idx) {
    // 既に充足しているかチェック
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (is_satisfied_by(i)) {
            return true;
        }
    }

    // 最後の未確定変数を充足方向に確定
    VariablePtr last_var = vars_[last_var_internal_idx];
    size_t last_lit = SIZE_MAX;

    for (size_t i = 0; i < n_pos_; ++i) {
        if (pos_[i] == last_var) {
            last_lit = i;
            break;
        }
    }
    if (last_lit == SIZE_MAX) {
        for (size_t i = 0; i < n_neg_; ++i) {
            if (neg_[i] == last_var) {
                last_lit = n_pos_ + i;
                break;
            }
        }
    }

    if (last_lit != SIZE_MAX) {
        model.enqueue_instantiate(last_var->id(), satisfying_value(last_lit));
    }

    return true;
}

void BoolClauseConstraint::check_initial_consistency() {
    // 全てのリテラルが充足不可能なら矛盾
    bool has_satisfiable = false;
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(i)) {
            has_satisfiable = true;
            break;
        }
    }
    if (!has_satisfiable) {
        set_initially_inconsistent(true);
    }
}

bool BoolClauseConstraint::can_satisfy(size_t lit_idx) const {
    if (lit_idx < n_pos_) {
        // 正リテラル: 1 になれるか（未確定 or = 1）
        return !pos_[lit_idx]->is_assigned() ||
               pos_[lit_idx]->assigned_value().value() == 1;
    } else {
        // 負リテラル: 0 になれるか（未確定 or = 0）
        size_t neg_idx = lit_idx - n_pos_;
        return !neg_[neg_idx]->is_assigned() ||
               neg_[neg_idx]->assigned_value().value() == 0;
    }
}

bool BoolClauseConstraint::is_satisfied_by(size_t lit_idx) const {
    if (lit_idx < n_pos_) {
        return pos_[lit_idx]->is_assigned() &&
               pos_[lit_idx]->assigned_value().value() == 1;
    } else {
        size_t neg_idx = lit_idx - n_pos_;
        return neg_[neg_idx]->is_assigned() &&
               neg_[neg_idx]->assigned_value().value() == 0;
    }
}

Domain::value_type BoolClauseConstraint::satisfying_value(size_t lit_idx) const {
    return (lit_idx < n_pos_) ? 1 : 0;
}

VariablePtr BoolClauseConstraint::get_var(size_t lit_idx) const {
    if (lit_idx < n_pos_) {
        return pos_[lit_idx];
    } else {
        return neg_[lit_idx - n_pos_];
    }
}

size_t BoolClauseConstraint::find_unwatched_candidate(size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        if (can_satisfy(i)) {
            return i;
        }
    }
    return SIZE_MAX;
}

void BoolClauseConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// BoolNotConstraint implementation
// ============================================================================

BoolNotConstraint::BoolNotConstraint(VariablePtr a, VariablePtr b)
    : Constraint({a, b})
    , a_(std::move(a))
    , b_(std::move(b)) {
    check_initial_consistency();
}

std::string BoolNotConstraint::name() const {
    return "bool_not";
}

std::vector<VariablePtr> BoolNotConstraint::variables() const {
    return {a_, b_};
}

std::optional<bool> BoolNotConstraint::is_satisfied() const {
    if (a_->is_assigned() && b_->is_assigned()) {
        // a + b = 1 (つまり a != b)
        return a_->assigned_value().value() != b_->assigned_value().value();
    }
    return std::nullopt;
}

bool BoolNotConstraint::presolve(Model& model) {
    // a が確定したら b を決定
    if (a_->is_assigned() && !b_->is_assigned()) {
        auto val = 1 - a_->assigned_value().value();
        if (!b_->domain().contains(val)) {
            return false;
        }
        b_->assign(val);
    }

    // b が確定したら a を決定
    if (b_->is_assigned() && !a_->is_assigned()) {
        auto val = 1 - b_->assigned_value().value();
        if (!a_->domain().contains(val)) {
            return false;
        }
        a_->assign(val);
    }

    return !a_->domain().empty() && !b_->domain().empty();
}

bool BoolNotConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    // 基底クラスの処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // a が確定したら b を決定（キューイング）
    if (a_->is_assigned() && !b_->is_assigned()) {
        auto val = 1 - a_->assigned_value().value();
        if (!b_->domain().contains(val)) {
            return false;
        }
        model.enqueue_instantiate(b_->id(), val);
    }

    // b が確定したら a を決定（キューイング）
    if (b_->is_assigned() && !a_->is_assigned()) {
        auto val = 1 - b_->assigned_value().value();
        if (!a_->domain().contains(val)) {
            return false;
        }
        model.enqueue_instantiate(a_->id(), val);
    }

    return true;
}

bool BoolNotConstraint::on_final_instantiate() {
    // a + b = 1 を確認
    return a_->assigned_value().value() != b_->assigned_value().value();
}

void BoolNotConstraint::check_initial_consistency() {
    // 両方が確定していて同じ値なら矛盾
    if (a_->is_assigned() && b_->is_assigned() &&
        a_->assigned_value() == b_->assigned_value()) {
        set_initially_inconsistent(true);
    }
}

} // namespace sabori_csp
