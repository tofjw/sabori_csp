/**
 * @file model.hpp
 * @brief CSPモデルクラス（変数・制約管理、集中Trail）
 */
#ifndef SABORI_CSP_MODEL_HPP
#define SABORI_CSP_MODEL_HPP

#include "sabori_csp/variable.hpp"
#include "sabori_csp/constraint.hpp"
#include <vector>
#include <map>
#include <string>
#include <variant>
#include <cstdint>

namespace sabori_csp {

/**
 * @brief 変数ドメイン用 Trail エントリ
 */
struct VarTrailEntry {
    size_t var_idx;
    int64_t old_min;
    int64_t old_max;
    size_t old_n;
};

/**
 * @brief 制約状態用 Trail エントリ
 *
 * 制約固有の状態を保存する。
 * - size_t: AllDifferent の pool_n など
 * - tuple<int64_t, int64_t, int64_t>: LinEq の (fixed_sum, min_pot, max_pot)
 */
struct ConstraintTrailEntry {
    size_t constraint_idx;
    std::variant<
        size_t,
        std::tuple<int64_t, int64_t, int64_t>
    > state;
};

/**
 * @brief 保留中のドメイン更新操作
 */
struct PendingUpdate {
    enum class Type { Instantiate, SetMin, SetMax, RemoveValue };
    Type type;
    size_t var_idx;
    Domain::value_type value;
};

/**
 * @brief CSPモデル
 *
 * 変数と制約を管理し、SoA形式で高速アクセス用データを保持する。
 * 集中型 Trail でバックトラックを効率化する。
 */
class Model {
public:
    Model() = default;

    // ===== 変数・制約管理 =====

    /**
     * @brief 変数を作成して登録（推奨）
     * @param name 変数名
     * @param domain 定義域
     * @return 作成された変数へのポインタ
     */
    VariablePtr create_variable(std::string name, Domain domain);

    /**
     * @brief 単一値（定数）変数を作成して登録
     * @param name 変数名
     * @param value 固定値
     * @return 作成された変数へのポインタ
     */
    VariablePtr create_variable(std::string name, Domain::value_type value);

    /**
     * @brief 区間ドメインの変数を作成して登録
     * @param name 変数名
     * @param min 下限
     * @param max 上限
     * @return 作成された変数へのポインタ
     */
    VariablePtr create_variable(std::string name, Domain::value_type min, Domain::value_type max);

    /**
     * @brief 値リストドメインの変数を作成して登録
     * @param name 変数名
     * @param values ドメイン値のリスト
     * @return 作成された変数へのポインタ
     */
    VariablePtr create_variable(std::string name, std::vector<Domain::value_type> values);

    /**
     * @brief 変数を追加（既存の変数を登録する場合）
     * @param var 追加する変数
     * @return 変数のID（インデックス）
     */
    size_t add_variable(VariablePtr var);

    /**
     * @brief 制約を追加
     * @param constraint 追加する制約
     */
    void add_constraint(ConstraintPtr constraint);

    /**
     * @brief 変数リストを取得
     */
    const std::vector<VariablePtr>& variables() const;

    /**
     * @brief 制約リストを取得
     */
    const std::vector<ConstraintPtr>& constraints() const;

    /**
     * @brief IDで変数を取得
     */
    VariablePtr variable(size_t id) const;

    /**
     * @brief 名前で変数を取得
     */
    VariablePtr variable(const std::string& name) const;

    // ===== SoA データアクセス =====

    /**
     * @brief SoA最小値配列を取得
     */
    const std::vector<Domain::value_type>& mins() const;
    std::vector<Domain::value_type>& mins();

    /**
     * @brief SoA最大値配列を取得
     */
    const std::vector<Domain::value_type>& maxs() const;
    std::vector<Domain::value_type>& maxs();

    /**
     * @brief SoAサイズ配列を取得
     */
    const std::vector<size_t>& sizes() const;
    std::vector<size_t>& sizes();

    /**
     * @brief 変数の最小値を取得
     */
    Domain::value_type var_min(size_t var_idx) const { return mins_[var_idx]; }

    /**
     * @brief 変数の最大値を取得
     */
    Domain::value_type var_max(size_t var_idx) const { return maxs_[var_idx]; }

    /**
     * @brief 変数のドメインサイズを取得
     */
    size_t var_size(size_t var_idx) const { return sizes_[var_idx]; }

    /**
     * @brief 変数が単一値に固定されているか
     */
    bool is_instantiated(size_t var_idx) const;

    /**
     * @brief 変数の値を取得（固定されている場合）
     */
    Domain::value_type value(size_t var_idx) const;

    /**
     * @brief 変数のドメインに値が含まれるか
     */
    bool contains(size_t var_idx, Domain::value_type val) const;

    // ===== ドメイン操作（Trail 付き） =====

    /**
     * @brief 変数の下限を更新
     * @param save_point バックトラック用セーブポイント
     * @param var_idx 変数インデックス
     * @param new_min 新しい下限
     * @return 成功（ドメインが空でない）したらtrue
     */
    bool set_min(int save_point, size_t var_idx, Domain::value_type new_min);

    /**
     * @brief 変数の上限を更新
     */
    bool set_max(int save_point, size_t var_idx, Domain::value_type new_max);

    /**
     * @brief 特定の値を削除
     */
    bool remove_value(int save_point, size_t var_idx, Domain::value_type value);

    /**
     * @brief 変数を特定の値に固定
     * @return 成功（値がドメインに存在）したらtrue
     */
    bool instantiate(int save_point, size_t var_idx, Domain::value_type value);

    // ===== Trail 管理 =====

    /**
     * @brief 変数状態を Trail に保存
     */
    void save_var_state(int save_point, size_t var_idx);

    /**
     * @brief 制約状態を Trail に保存
     */
    template<typename StateT>
    void save_constraint_state(int save_point, size_t constraint_idx, StateT state);

    /**
     * @brief 指定セーブポイントまで巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 制約が状態を変更したことを記録
     * @param constraint_idx 制約のインデックス
     * @param save_point 変更時のセーブポイント
     */
    void mark_constraint_dirty(size_t constraint_idx, int save_point);

    /**
     * @brief dirty な制約の rewind_to を呼び出し
     * @param save_point 復元先のセーブポイント
     */
    void rewind_dirty_constraints(int save_point);

    /**
     * @brief 変数 Trail のサイズを取得
     */
    size_t var_trail_size() const;

    /**
     * @brief 制約 Trail のサイズを取得
     */
    size_t constraint_trail_size() const;

    // ===== 同期 =====

    /**
     * @brief DomainからSoAデータを同期
     */
    void sync_from_domains();

    /**
     * @brief SoAデータからDomainを同期
     */
    void sync_to_domains();

    // ===== 伝播キュー =====

    /**
     * @brief 変数の確定をキューに追加（制約から呼び出される）
     */
    void enqueue_instantiate(size_t var_idx, Domain::value_type value);

    /**
     * @brief 下限設定をキューに追加
     */
    void enqueue_set_min(size_t var_idx, Domain::value_type new_min);

    /**
     * @brief 上限設定をキューに追加
     */
    void enqueue_set_max(size_t var_idx, Domain::value_type new_max);

    /**
     * @brief 値除去をキューに追加
     */
    void enqueue_remove_value(size_t var_idx, Domain::value_type value);

    /**
     * @brief 保留中の更新操作を取得
     */
    const std::vector<PendingUpdate>& pending_updates() const;

    /**
     * @brief 保留中の更新操作をクリア
     */
    void clear_pending_updates();

private:
    std::vector<VariablePtr> variables_;
    std::vector<ConstraintPtr> constraints_;
    std::map<std::string, size_t> name_to_id_;

    // 変数IDカウンタ
    size_t next_var_id_ = 0;

    // SoA データ（高速アクセス用）
    std::vector<Domain::value_type> mins_;
    std::vector<Domain::value_type> maxs_;
    std::vector<size_t> sizes_;

    // 集中 Trail
    std::vector<std::pair<int, VarTrailEntry>> var_trail_;
    std::vector<std::pair<int, ConstraintTrailEntry>> constraint_trail_;
    std::vector<std::pair<int, size_t>> dirty_constraint_trail_;  // (save_point, constraint_idx)

    // 最後に保存した変数ごとのセーブポイント（重複保存防止）
    std::vector<int> last_saved_level_;

    // 伝播キュー（制約が追加した保留中のドメイン更新操作）
    std::vector<PendingUpdate> pending_updates_;

    // 制約ウォッチリスト: 各変数に関連する制約のリスト
    std::vector<std::vector<size_t>> var_to_constraint_indices_;

public:
    /**
     * @brief 変数に関連する制約インデックスを取得
     */
    const std::vector<size_t>& constraints_for_var(size_t var_idx) const {
        static const std::vector<size_t> empty;
        if (var_idx < var_to_constraint_indices_.size()) {
            return var_to_constraint_indices_[var_idx];
        }
        return empty;
    }

    /**
     * @brief 制約ウォッチリストを構築（制約追加後に呼び出す）
     */
    void build_constraint_watch_list();

    /**
     * @brief 全制約の事前解決を実行
     *
     * 全制約が追加された後、探索開始前に呼び出す。
     * 各制約の presolve() を順番に呼び出し、内部状態を初期化する。
     *
     * @return 全制約の presolve が成功すれば true、矛盾検出時は false
     */
    bool presolve();
};

// テンプレート実装
template<typename StateT>
void Model::save_constraint_state(int save_point, size_t constraint_idx, StateT state) {
    ConstraintTrailEntry entry;
    entry.constraint_idx = constraint_idx;
    entry.state = state;
    constraint_trail_.push_back({save_point, entry});
}

} // namespace sabori_csp

#endif // SABORI_CSP_MODEL_HPP
