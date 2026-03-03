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
    : Constraint()
    , n_(x.size())
    , strict_(strict)
{
    // var_ids_ = [x0..xn-1, y0..yn-1, dx0..dxn-1, dy0..dyn-1]
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(4 * n_);
    for (auto& v : x)  all_vars.push_back(std::move(v));
    for (auto& v : y)  all_vars.push_back(std::move(v));
    for (auto& v : dx) all_vars.push_back(std::move(v));
    for (auto& v : dy) all_vars.push_back(std::move(v));

    var_ids_ = extract_var_ids(all_vars);
}

std::string DiffnConstraint::name() const {
    return strict_ ? "fzn_diffn" : "fzn_diffn_nonstrict";
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

// ---------- Pairwise propagation (presolve版: model.variable() 経由) ----------

bool DiffnConstraint::propagate_pairwise_direct(Model& model) {
    for (size_t i = 0; i < n_; ++i) {
        auto xi_var  = model.variable(var_ids_[i]);
        auto yi_var  = model.variable(var_ids_[n_ + i]);
        auto dxi_var = model.variable(var_ids_[2 * n_ + i]);
        auto dyi_var = model.variable(var_ids_[3 * n_ + i]);

        auto xi_min  = xi_var->min();
        auto xi_max  = xi_var->max();
        auto yi_min  = yi_var->min();
        auto yi_max  = yi_var->max();
        auto dxi_min = dxi_var->min();
        auto dyi_min = dyi_var->min();

        if (!strict_ && (dxi_min == 0 || dyi_min == 0)) continue;

        for (size_t j = i + 1; j < n_; ++j) {
            auto xj_var  = model.variable(var_ids_[j]);
            auto yj_var  = model.variable(var_ids_[n_ + j]);
            auto dxj_var = model.variable(var_ids_[2 * n_ + j]);
            auto dyj_var = model.variable(var_ids_[3 * n_ + j]);

            auto xj_min  = xj_var->min();
            auto xj_max  = xj_var->max();
            auto yj_min  = yj_var->min();
            auto yj_max  = yj_var->max();
            auto dxj_min = dxj_var->min();
            auto dyj_min = dyj_var->min();

            if (!strict_ && (dxj_min == 0 || dyj_min == 0)) continue;

            bool can_left  = (xi_min + dxi_min <= xj_max);
            bool can_right = (xj_min + dxj_min <= xi_max);
            bool can_below = (yi_min + dyi_min <= yj_max);
            bool can_above = (yj_min + dyj_min <= yi_max);

            int directions = can_left + can_right + can_below + can_above;

            if (directions == 0) return false;

            if (directions == 1) {
                if (can_left) {
                    if (!xj_var->remove_below(xi_min + dxi_min)) return false;
                    if (!xi_var->remove_above(xj_max - dxi_min)) return false;
                } else if (can_right) {
                    if (!xi_var->remove_below(xj_min + dxj_min)) return false;
                    if (!xj_var->remove_above(xi_max - dxj_min)) return false;
                } else if (can_below) {
                    if (!yj_var->remove_below(yi_min + dyi_min)) return false;
                    if (!yi_var->remove_above(yj_max - dyi_min)) return false;
                } else {  // can_above
                    if (!yi_var->remove_below(yj_min + dyj_min)) return false;
                    if (!yj_var->remove_above(yi_max - dyj_min)) return false;
                }
            }
        }
    }
    return true;
}

// ---------- Presolve ----------

PresolveResult DiffnConstraint::presolve(Model& model) {
    // Snapshot domain sizes before propagation
    size_t total_size_before = 0;
    for (size_t vid : var_ids_) {
        total_size_before += model.variable(vid)->domain().size();
    }
    if (!propagate_pairwise_direct(model)) return PresolveResult::Contradiction;
    size_t total_size_after = 0;
    for (size_t vid : var_ids_) {
        total_size_after += model.variable(vid)->domain().size();
    }
    return (total_size_after < total_size_before) ? PresolveResult::Changed : PresolveResult::Unchanged;
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

    if (!has_uninstantiated(model)) {
        return on_final_instantiate(model);
    }

    return propagate_pairwise(model);
}

bool DiffnConstraint::on_final_instantiate(const Model& model) {
    for (size_t i = 0; i < n_; ++i) {
        auto xi  = model.value(var_ids_[i]);
        auto yi  = model.value(var_ids_[n_ + i]);
        auto dxi = model.value(var_ids_[2 * n_ + i]);
        auto dyi = model.value(var_ids_[3 * n_ + i]);
        for (size_t j = i + 1; j < n_; ++j) {
            auto xj  = model.value(var_ids_[j]);
            auto yj  = model.value(var_ids_[n_ + j]);
            auto dxj = model.value(var_ids_[2 * n_ + j]);
            auto dyj = model.value(var_ids_[3 * n_ + j]);

            // nonstrict: skip zero-size rectangles
            if (!strict_ && ((dxi == 0 || dyi == 0) || (dxj == 0 || dyj == 0)))
                continue;

            // overlap check: not separated in any of 4 directions means overlap
            bool separated = (xi + dxi <= xj) || (xj + dxj <= xi) ||
                             (yi + dyi <= yj) || (yj + dyj <= yi);
            if (!separated) return false;
        }
    }
    return true;
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

}  // namespace sabori_csp
