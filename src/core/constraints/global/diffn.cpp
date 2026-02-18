#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"

namespace sabori_csp {

// ============================================================================
// DiffnConstraint — pairwise obligatory region propagation
// ============================================================================

DiffnConstraint::DiffnConstraint(
    std::vector<VariablePtr> x, std::vector<VariablePtr> y,
    std::vector<VariablePtr> dx, std::vector<VariablePtr> dy,
    bool strict)
    : Constraint(std::vector<VariablePtr>())
    , n_(x.size())
    , strict_(strict)
{
    // vars_ = [x0..xn-1, y0..yn-1, dx0..dxn-1, dy0..dyn-1]
    vars_.reserve(4 * n_);
    for (auto& v : x)  vars_.push_back(std::move(v));
    for (auto& v : y)  vars_.push_back(std::move(v));
    for (auto& v : dx) vars_.push_back(std::move(v));
    for (auto& v : dy) vars_.push_back(std::move(v));

    update_var_ids();
    check_initial_consistency();
}

std::string DiffnConstraint::name() const {
    return strict_ ? "fzn_diffn" : "fzn_diffn_nonstrict";
}

std::vector<VariablePtr> DiffnConstraint::variables() const {
    return vars_;
}

std::optional<bool> DiffnConstraint::is_satisfied() const {
    for (const auto& var : vars_) {
        if (!var->is_assigned()) return std::nullopt;
    }
    for (size_t i = 0; i < n_; ++i) {
        auto xi  = vars_[i]->assigned_value().value();
        auto yi  = vars_[n_ + i]->assigned_value().value();
        auto dxi = vars_[2 * n_ + i]->assigned_value().value();
        auto dyi = vars_[3 * n_ + i]->assigned_value().value();
        for (size_t j = i + 1; j < n_; ++j) {
            auto xj  = vars_[j]->assigned_value().value();
            auto yj  = vars_[n_ + j]->assigned_value().value();
            auto dxj = vars_[2 * n_ + j]->assigned_value().value();
            auto dyj = vars_[3 * n_ + j]->assigned_value().value();

            // nonstrict: ゼロサイズ矩形はスキップ
            if (!strict_ && ((dxi == 0 || dyi == 0) || (dxj == 0 || dyj == 0)))
                continue;

            // 重複チェック: 4方向いずれの分離も成立しなければ重複
            bool separated = (xi + dxi <= xj) || (xj + dxj <= xi) ||
                             (yi + dyi <= yj) || (yj + dyj <= yi);
            if (!separated) return false;
        }
    }
    return true;
}

// ---------- Pairwise propagation (callback版: model 経由) ----------

bool DiffnConstraint::propagate_pairwise(Model& model) {
    for (size_t i = 0; i < n_; ++i) {
        auto x_i  = var_ids_[i];
        auto y_i  = var_ids_[n_ + i];
        auto dx_i = var_ids_[2 * n_ + i];
        auto dy_i = var_ids_[3 * n_ + i];

        auto xi_min  = model.var_min(x_i);
        auto xi_max  = model.var_max(x_i);
        auto yi_min  = model.var_min(y_i);
        auto yi_max  = model.var_max(y_i);
        auto dxi_min = model.var_min(dx_i);
        auto dyi_min = model.var_min(dy_i);

        // nonstrict: サイズ 0 の矩形はスキップ
        if (!strict_ && (dxi_min == 0 || dyi_min == 0)) continue;

        for (size_t j = i + 1; j < n_; ++j) {
            auto x_j  = var_ids_[j];
            auto y_j  = var_ids_[n_ + j];
            auto dx_j = var_ids_[2 * n_ + j];
            auto dy_j = var_ids_[3 * n_ + j];

            auto xj_min  = model.var_min(x_j);
            auto xj_max  = model.var_max(x_j);
            auto yj_min  = model.var_min(y_j);
            auto yj_max  = model.var_max(y_j);
            auto dxj_min = model.var_min(dx_j);
            auto dyj_min = model.var_min(dy_j);

            // nonstrict: サイズ 0 の矩形はスキップ
            if (!strict_ && (dxj_min == 0 || dyj_min == 0)) continue;

            // 4方向の分離可能性チェック
            bool can_left  = (xi_min + dxi_min <= xj_max);  // i が j の左
            bool can_right = (xj_min + dxj_min <= xi_max);  // i が j の右
            bool can_below = (yi_min + dyi_min <= yj_max);  // i が j の下
            bool can_above = (yj_min + dyj_min <= yi_max);  // i が j の上

            int directions = can_left + can_right + can_below + can_above;

            if (directions == 0) return false;  // 分離不可能 → 矛盾

            if (directions == 1) {
                // 強制分離: 1方向のみ可能 → bounds tightening
                if (can_left) {
                    // i が j の左に強制: x[i] + dx[i] <= x[j]
                    // → x[j] >= x[i].min + dx[i].min
                    // → x[i] <= x[j].max - dx[i].min
                    model.enqueue_set_min(x_j, xi_min + dxi_min);
                    model.enqueue_set_max(x_i, xj_max - dxi_min);
                } else if (can_right) {
                    // i が j の右に強制: x[j] + dx[j] <= x[i]
                    model.enqueue_set_min(x_i, xj_min + dxj_min);
                    model.enqueue_set_max(x_j, xi_max - dxj_min);
                } else if (can_below) {
                    // i が j の下に強制: y[i] + dy[i] <= y[j]
                    model.enqueue_set_min(y_j, yi_min + dyi_min);
                    model.enqueue_set_max(y_i, yj_max - dyi_min);
                } else {  // can_above
                    // i が j の上に強制: y[j] + dy[j] <= y[i]
                    model.enqueue_set_min(y_i, yj_min + dyj_min);
                    model.enqueue_set_max(y_j, yi_max - dyj_min);
                }
            }
            // directions >= 2: 伝播なし
        }
    }
    return true;
}

// ---------- Pairwise propagation (presolve版: vars_ 直接) ----------

bool DiffnConstraint::propagate_pairwise_direct() {
    for (size_t i = 0; i < n_; ++i) {
        auto xi_min  = vars_[i]->min();
        auto xi_max  = vars_[i]->max();
        auto yi_min  = vars_[n_ + i]->min();
        auto yi_max  = vars_[n_ + i]->max();
        auto dxi_min = vars_[2 * n_ + i]->min();
        auto dyi_min = vars_[3 * n_ + i]->min();

        if (!strict_ && (dxi_min == 0 || dyi_min == 0)) continue;

        for (size_t j = i + 1; j < n_; ++j) {
            auto xj_min  = vars_[j]->min();
            auto xj_max  = vars_[j]->max();
            auto yj_min  = vars_[n_ + j]->min();
            auto yj_max  = vars_[n_ + j]->max();
            auto dxj_min = vars_[2 * n_ + j]->min();
            auto dyj_min = vars_[3 * n_ + j]->min();

            if (!strict_ && (dxj_min == 0 || dyj_min == 0)) continue;

            bool can_left  = (xi_min + dxi_min <= xj_max);
            bool can_right = (xj_min + dxj_min <= xi_max);
            bool can_below = (yi_min + dyi_min <= yj_max);
            bool can_above = (yj_min + dyj_min <= yi_max);

            int directions = can_left + can_right + can_below + can_above;

            if (directions == 0) return false;

            if (directions == 1) {
                if (can_left) {
                    if (!vars_[j]->remove_below(xi_min + dxi_min)) return false;
                    if (!vars_[i]->remove_above(xj_max - dxi_min)) return false;
                } else if (can_right) {
                    if (!vars_[i]->remove_below(xj_min + dxj_min)) return false;
                    if (!vars_[j]->remove_above(xi_max - dxj_min)) return false;
                } else if (can_below) {
                    if (!vars_[n_ + j]->remove_below(yi_min + dyi_min)) return false;
                    if (!vars_[n_ + i]->remove_above(yj_max - dyi_min)) return false;
                } else {  // can_above
                    if (!vars_[n_ + i]->remove_below(yj_min + dyj_min)) return false;
                    if (!vars_[n_ + j]->remove_above(yi_max - dyj_min)) return false;
                }
            }
        }
    }
    return true;
}

// ---------- Presolve ----------

bool DiffnConstraint::presolve(Model& model) {
    return propagate_pairwise_direct();
}

// ---------- Prepare propagation ----------

bool DiffnConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

// ---------- Event handlers ----------

bool DiffnConstraint::on_instantiate(
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

    if (!has_uninstantiated()) {
        return on_final_instantiate();
    }

    return propagate_pairwise(model);
}

bool DiffnConstraint::on_final_instantiate() {
    auto result = is_satisfied();
    return result.has_value() && result.value();
}

bool DiffnConstraint::on_set_min(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/)
{
    return propagate_pairwise(model);
}

bool DiffnConstraint::on_set_max(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/)
{
    return propagate_pairwise(model);
}

// ---------- Trail ----------

void DiffnConstraint::rewind_to(int /*save_point*/) {
    // 状態なし: 何もしない
}

// ---------- Initial consistency ----------

void DiffnConstraint::check_initial_consistency() {
    // エネルギーチェック: 最小面積合計 vs 外接矩形面積
    int64_t total_area = 0;
    int64_t global_x_min = 0, global_x_max = 0;
    int64_t global_y_min = 0, global_y_max = 0;
    bool has_relevant = false;

    for (size_t i = 0; i < n_; ++i) {
        int64_t dxi_min = vars_[2 * n_ + i]->min();
        int64_t dyi_min = vars_[3 * n_ + i]->min();
        if (!strict_ && (dxi_min == 0 || dyi_min == 0)) continue;
        if (dxi_min <= 0 || dyi_min <= 0) continue;

        total_area += dxi_min * dyi_min;

        int64_t xi_min = vars_[i]->min();
        int64_t xi_max = vars_[i]->max() + vars_[2 * n_ + i]->max();
        int64_t yi_min = vars_[n_ + i]->min();
        int64_t yi_max = vars_[n_ + i]->max() + vars_[3 * n_ + i]->max();

        if (!has_relevant) {
            global_x_min = xi_min;
            global_x_max = xi_max;
            global_y_min = yi_min;
            global_y_max = yi_max;
            has_relevant = true;
        } else {
            if (xi_min < global_x_min) global_x_min = xi_min;
            if (xi_max > global_x_max) global_x_max = xi_max;
            if (yi_min < global_y_min) global_y_min = yi_min;
            if (yi_max > global_y_max) global_y_max = yi_max;
        }
    }

    if (has_relevant) {
        int64_t bounding_area = (global_x_max - global_x_min) *
                                (global_y_max - global_y_min);
        if (total_area > bounding_area) {
            set_initially_inconsistent(true);
        }
    }
}

}  // namespace sabori_csp
