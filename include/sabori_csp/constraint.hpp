/**
 * @file constraint.hpp
 * @brief 制約基底クラスと全制約ヘッダのインクルード
 */
#ifndef SABORI_CSP_CONSTRAINT_HPP
#define SABORI_CSP_CONSTRAINT_HPP

#include "sabori_csp/variable.hpp"
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

namespace sabori_csp {

// Forward declaration
class Model;

/**
 * @brief 制約の基底クラス
 *
 * 2-Watched Literal (2WL) による効率的な伝播監視を提供する。
 * 制約が関与する変数のうち、未確定の2つだけを監視し、
 * それらが確定した時のみ伝播処理を行う。
 */
class Constraint {
public:
    virtual ~Constraint() = default;

    /**
     * @brief 制約IDを取得
     */
    size_t id() const { return id_; }

    /**
     * @brief 制約の名前を取得
     */
    virtual std::string name() const = 0;

    /**
     * @brief 制約が関係する変数を取得
     */
    virtual std::vector<VariablePtr> variables() const = 0;

    /**
     * @brief 制約が満たされているか確認
     * @return 満たされていればtrue、違反していればfalse、
     *         未確定ならstd::nullopt
     */
    virtual std::optional<bool> is_satisfied() const = 0;

    /**
     * @brief 制約伝播を実行（レガシー、後方互換性用）
     * @return 伝播が成功すればtrue、失敗（定義域が空）すればfalse
     */
    virtual bool propagate() = 0;

    // ===== 2-Watched Literal (2WL) インターフェース =====

    /**
     * @brief 変数が確定した時に呼ばれる
     *
     * 2WL に基づき、監視変数が確定したら別の未確定変数に監視を移す。
     * 全ての変数が確定したら on_final_instantiate() を呼び出す。
     *
     * @param model モデルへの参照
     * @param save_point バックトラック用セーブポイント
     * @param var_idx 確定した変数のインデックス
     * @param value 確定した値
     * @param prev_min 確定前の最小値
     * @param prev_max 確定前の最大値
     * @return 伝播が成功すればtrue、失敗すればfalse
     */
    virtual bool on_instantiate(Model& model, int save_point,
                                size_t var_idx, Domain::value_type value,
                                Domain::value_type prev_min, Domain::value_type prev_max);

    /**
     * @brief 全変数確定時の最終チェック
     *
     * サブクラスでオーバーライドして制約固有のチェックを行う。
     * デフォルトでは is_satisfied() を使用する。
     *
     * @return 制約が満たされていればtrue
     */
    virtual bool on_final_instantiate();

    /**
     * @brief 制約設定直後に全変数が確定しているかどうか
     */
    bool can_be_finalized() const;

    /**
     * @brief 監視変数1のインデックスを取得
     */
    int watch1() const { return w1_; }

    /**
     * @brief 監視変数2のインデックスを取得
     */
    int watch2() const { return w2_; }

    /**
     * @brief 2WL を初期化
     *
     * 変数リストから未確定の2変数を選んで監視する。
     */
    void init_watches();

protected:
    /**
     * @brief コンストラクタ
     *
     * サブクラスから呼び出す。IDを自動付与し、2WLを初期化する。
     *
     * @param vars 制約に関与する変数リスト
     */
    explicit Constraint(const std::vector<VariablePtr>& vars);

    /**
     * @brief デフォルトコンストラクタ（後方互換性用）
     */
    Constraint();

    /**
     * @brief 変数リストを設定（遅延初期化用）
     */
    void set_variables(const std::vector<VariablePtr>& vars);

    // 制約に関与する変数（サブクラスで管理してもよい）
    std::vector<VariablePtr> vars_;

private:
    static size_t next_id_;
    size_t id_;

    // 2-Watched Literal
    int w1_ = -1;  // 監視変数1のインデックス
    int w2_ = -1;  // 監視変数2のインデックス
};

using ConstraintPtr = std::shared_ptr<Constraint>;

} // namespace sabori_csp

// 各制約グループのヘッダをインクルード
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/global.hpp"

#endif // SABORI_CSP_CONSTRAINT_HPP
