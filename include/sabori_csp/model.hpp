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
 * @brief 変数データ（AoS 構造体）
 *
 * 同一変数の min/max/size を同一キャッシュラインに配置する。
 */
struct VarData {
    Domain::value_type min;
    Domain::value_type max;
    size_t size;
    size_t initial_range;
    Domain::value_type support_value;
    int last_saved_level = -1;
    bool is_defined_var = false;
};

/**
 * @brief 変数ドメイン用 Trail エントリ
 */
struct VarTrailEntry {
    size_t var_idx;
    int64_t old_min;
    int64_t old_max;
    size_t old_n;
    size_t old_removed_count;  // bounds-only 用（sparse-set では 0）
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
 * 変数と制約を管理し、AoS形式（VarData）で高速アクセス用データを保持する。
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
     * @brief 変数エイリアスを追加（bool2int等で使用）
     * @param alias_name エイリアス名
     * @param var_id エイリアス先の変数ID
     */
    void add_variable_alias(const std::string& alias_name, size_t var_id);

    /**
     * @brief 変数エイリアスマップを取得
     */
    const std::map<std::string, size_t>& variable_aliases() const;

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

    /**
     * @brief 名前から変数インデックスを検索（エイリアスも考慮）
     * @return 見つかればインデックス、なければ SIZE_MAX
     */
    size_t find_variable_index(const std::string& name) const;

    // ===== 変数データアクセス =====

    /**
     * @brief 変数の最小値を取得
     */
    Domain::value_type var_min(size_t var_idx) const { return var_data_[var_idx].min; }

    /**
     * @brief 変数の最大値を取得
     */
    Domain::value_type var_max(size_t var_idx) const { return var_data_[var_idx].max; }

    /**
     * @brief 変数のドメインサイズを取得（sparse set の物理サイズ）
     */
    size_t var_size(size_t var_idx) const { return var_data_[var_idx].size; }

    /**
     * @brief 変数の初期レンジを取得
     */
    size_t initial_range(size_t var_idx) const { return var_data_[var_idx].initial_range; }

    /**
     * @brief 変数が単一値に固定されているか
     */
    bool is_instantiated(size_t var_idx) const { return var_data_[var_idx].min == var_data_[var_idx].max; }

    /**
     * @brief instantiated な変数の数を取得（O(1)）
     */
    size_t instantiated_count() const { return instantiated_count_; }

    /**
     * @brief 変数の値を取得（固定されている場合）
     */
    Domain::value_type value(size_t var_idx) const { return var_data_[var_idx].min; }

    /**
     * @brief 変数のドメインに値が含まれるか
     */
    bool contains(size_t var_idx, Domain::value_type val) const;

    /**
     * @brief 変数データへの参照を取得（Variable::sync_soa 等で使用）
     */
    VarData& var_data(size_t var_idx) { return var_data_[var_idx]; }
    const VarData& var_data(size_t var_idx) const { return var_data_[var_idx]; }

    /**
     * @brief 変数が is_defined_var か
     */
    bool is_defined_var(size_t var_idx) const { return var_data_[var_idx].is_defined_var; }

    /**
     * @brief 変数を is_defined_var としてマーク
     */
    void set_defined_var(size_t var_idx);

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
     * @brief 保留中の更新操作があるか
     */
    bool has_pending_updates() const { return pending_read_idx_ < pending_updates_.size(); }

    /**
     * @brief 保留中の更新操作を1つ取り出す
     */
    PendingUpdate pop_pending_update() { return pending_updates_[pending_read_idx_++]; }

    /**
     * @brief 保留中の更新操作をクリア
     */
    void clear_pending_updates();

private:
    std::vector<VariablePtr> variables_;
    std::vector<ConstraintPtr> constraints_;
    std::map<std::string, size_t> name_to_id_;
    std::map<std::string, size_t> variable_aliases_;  // alias_name -> var_id

    // 変数IDカウンタ
    size_t next_var_id_ = 0;

    // 変数データ（AoS: 同一変数の min/max/size が同一キャッシュラインに乗る）
    std::vector<VarData> var_data_;

    // 集中 Trail
    std::vector<std::pair<int, VarTrailEntry>> var_trail_;
    std::vector<std::pair<int, ConstraintTrailEntry>> constraint_trail_;
    std::vector<std::pair<int, size_t>> dirty_constraint_trail_;  // (save_point, constraint_idx)

    // instantiated 変数カウンタ（O(1) 参照用）
    size_t instantiated_count_ = 0;

    // 伝播キュー（制約が追加した保留中のドメイン更新操作）
    std::vector<PendingUpdate> pending_updates_;
    size_t pending_read_idx_ = 0;

    // 制約 raw ポインタ配列（shared_ptr デリファレンス回避）
    std::vector<Constraint*> constraint_ptrs_;

    // 制約ウォッチリスト: 各変数に関連する制約のリスト
    struct WatchEntry {
        size_t constraint_idx;
        size_t internal_var_idx;
    };
    std::vector<std::vector<WatchEntry>> var_to_constraint_indices_;

public:
    /**
     * @brief 変数に関連する制約インデックスを取得
     */
    const std::vector<WatchEntry>& constraints_for_var(size_t var_idx) const {
        static const std::vector<WatchEntry> empty;
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
     * @brief 伝播準備: 各制約の内部状態を初期化
     *
     * 全制約が追加された後、探索開始前に呼び出す。
     * 各制約の prepare_propagation() を順番に呼び出し、内部状態を初期化する。
     * その後 SoA データを同期する。
     *
     * @return 全制約の prepare_propagation が成功すれば true、矛盾検出時は false
     */
    bool prepare_propagation();
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
