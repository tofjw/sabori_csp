#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

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

namespace {

// DomainWriter 抽象: 読み出し元（var_data_ 経由 / Domain 直接）と書き込み方式
// （enqueue / 即時 remove）の差だけをポリシーに切り出し、pairwise 分離ロジック本体を
// 1 箇所に集約する。テンプレートなので hot path（伝播版）に仮想呼び出しは入らない。

// 伝播版: bounds は var_data_（model.var_min/max）、書き込みは enqueue（矛盾は
// solver がキュー処理時に検出するため set_* は常に成功扱い）。
struct EnqueueAccess {
    Model& model;
    Domain::value_type lo(size_t vid) const { return model.var_min(vid); }
    Domain::value_type hi(size_t vid) const { return model.var_max(vid); }
    bool set_min(size_t vid, Domain::value_type v) { model.enqueue_set_min(vid, v); return true; }
    bool set_max(size_t vid, Domain::value_type v) { model.enqueue_set_max(vid, v); return true; }
};

// presolve 版: bounds は Domain 直接（var_data_ は presolve 中ラグするため）、書き込みは
// 即時 remove（false で矛盾を即検出）。
struct DirectAccess {
    Model& model;
    Domain::value_type lo(size_t vid) const { return model.variable(vid)->min(); }
    Domain::value_type hi(size_t vid) const { return model.variable(vid)->max(); }
    bool set_min(size_t vid, Domain::value_type v) { return model.variable(vid)->remove_below(v); }
    bool set_max(size_t vid, Domain::value_type v) { return model.variable(vid)->remove_above(v); }
};

// 矩形ペア (i, j) の obligatory-region 分離。分離可能方向を数え、1方向のみなら
// その方向へ bounds を絞る。読み書きは Acc に委譲（伝播版/presolve 版で共通）。
template <class Acc>
bool diffn_pair(const std::vector<size_t>& var_ids, size_t n, bool strict, Acc& a,
                size_t i, size_t j) {
    auto x_i  = var_ids[i];
    auto y_i  = var_ids[n + i];
    auto dx_i = var_ids[2 * n + i];
    auto dy_i = var_ids[3 * n + i];

    auto xi_min  = a.lo(x_i);
    auto xi_max  = a.hi(x_i);
    auto yi_min  = a.lo(y_i);
    auto yi_max  = a.hi(y_i);
    auto dxi_min = a.lo(dx_i);
    auto dyi_min = a.lo(dy_i);

    // nonstrict: サイズ 0 の矩形はスキップ
    if (!strict && (dxi_min == 0 || dyi_min == 0)) return true;

    auto x_j  = var_ids[j];
    auto y_j  = var_ids[n + j];
    auto dx_j = var_ids[2 * n + j];
    auto dy_j = var_ids[3 * n + j];

    auto xj_min  = a.lo(x_j);
    auto xj_max  = a.hi(x_j);
    auto yj_min  = a.lo(y_j);
    auto yj_max  = a.hi(y_j);
    auto dxj_min = a.lo(dx_j);
    auto dyj_min = a.lo(dy_j);

    if (!strict && (dxj_min == 0 || dyj_min == 0)) return true;

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
            if (!a.set_min(x_j, xi_min + dxi_min)) return false;
            if (!a.set_max(x_i, xj_max - dxi_min)) return false;
        } else if (can_right) {
            // i が j の右に強制: x[j] + dx[j] <= x[i]
            if (!a.set_min(x_i, xj_min + dxj_min)) return false;
            if (!a.set_max(x_j, xi_max - dxj_min)) return false;
        } else if (can_below) {
            // i が j の下に強制: y[i] + dy[i] <= y[j]
            if (!a.set_min(y_j, yi_min + dyi_min)) return false;
            if (!a.set_max(y_i, yj_max - dyi_min)) return false;
        } else {  // can_above
            // i が j の上に強制: y[j] + dy[j] <= y[i]
            if (!a.set_min(y_i, yj_min + dyj_min)) return false;
            if (!a.set_max(y_j, yi_max - dyj_min)) return false;
        }
    }
    // directions >= 2: 伝播なし
    return true;
}

/// 全ペア走査 O(n^2)。presolve と、変化した矩形が特定できない場合に使う。
template <class Acc>
bool diffn_pairwise(const std::vector<size_t>& var_ids, size_t n, bool strict, Acc a) {
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (!diffn_pair(var_ids, n, strict, a, i, j)) return false;
        }
    }
    return true;
}

/**
 * @brief 矩形 r を含むペアだけを走査する O(n) 版
 *
 * 矩形 r の bounds が変わったとき、判定が変わりうるのはペア (r, *) に限られる。
 * ペア (j, k)（r を含まない）は j か k が最後に変化した時点で走査済みで、
 * その後 bounds が動いていないなら結果も同じ（= 新しい伝播は出ない）。
 * 各変数の変化は必ず自分のコールバックを呼ぶので、この分割で不動点は変わらない。
 */
template <class Acc>
bool diffn_pairwise_one(const std::vector<size_t>& var_ids, size_t n, bool strict,
                        Acc a, size_t r) {
    for (size_t j = 0; j < n; ++j) {
        if (j == r) continue;
        // 引数順は (小さい方, 大きい方) に揃える（分離方向の意味は対称だが、
        // 全走査版と同じ評価順・同じ書き込み順にして挙動を一致させる）。
        const size_t i0 = std::min(r, j), j0 = std::max(r, j);
        if (!diffn_pair(var_ids, n, strict, a, i0, j0)) return false;
    }
    return true;
}

}  // namespace

bool DiffnConstraint::propagate_pairwise(Model& model) {
    return diffn_pairwise(var_ids_, n_, strict_, EnqueueAccess{model});
}

bool DiffnConstraint::propagate_pairwise_for(Model& model, size_t rect_idx) {
    return diffn_pairwise_one(var_ids_, n_, strict_, EnqueueAccess{model}, rect_idx);
}

// ---------- Pairwise propagation (presolve版: Domain 直接 + 即時 remove) ----------

bool DiffnConstraint::propagate_pairwise_direct(Model& model) {
    return diffn_pairwise(var_ids_, n_, strict_, DirectAccess{model});
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
    size_t internal_var_idx,
    Domain::value_type value,
    Domain::value_type prev_min, Domain::value_type prev_max)
{
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    if (!has_uninstantiated(model)) {
        return on_final_instantiate(model);
    }

    // 変化したのは矩形 (internal_var_idx % n_) だけなので、そのペアだけ見る。
    return propagate_pairwise_for(model, internal_var_idx % n_);
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
    size_t internal_var_idx,
    Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/)
{
    return propagate_pairwise_for(model, internal_var_idx % n_);
}

bool DiffnConstraint::on_set_max(
    Model& model, int save_point,
    size_t internal_var_idx,
    Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/)
{
    return propagate_pairwise_for(model, internal_var_idx % n_);
}

// ---------- Trail ----------

void DiffnConstraint::rewind_to(int /*save_point*/) {
    // 状態なし: 何もしない
}

}  // namespace sabori_csp
