#ifndef SABORI_CSP_CONSTRAINT_TRAIL_HPP
#define SABORI_CSP_CONSTRAINT_TRAIL_HPP

#include <utility>
#include <vector>

namespace sabori_csp {

/**
 * @brief 制約内部状態のセーブポイント付き trail
 *
 * 多くの制約（int_lin_* 系など）は、伝播で更新する内部状態（部分和・残ポテンシャル・
 * 未確定数等）をバックトラックで巻き戻すために (save_point, 状態スナップショット) の
 * スタックを持つ。その「同一セーブポイントでは一度だけ保存」「save_point 超えを巻き戻す」
 * という定型ロジックを共通化する。
 *
 * 状態スナップショットの型 @p State は制約ごとに異なる小さな POD（フィールドの集合）で、
 * 各フィールドへの読み書きは呼び出し側が save_if_needed の引数と rewind_to の復元
 * コールバックで明示する。これにより本テンプレートは「どのフィールドを trail するか」に
 * 依存しない。
 *
 * @tparam State 1セーブポイント分の状態スナップショット型
 */
template <typename State>
class ConstraintTrail {
public:
    /**
     * @brief 現セーブポイントの状態をまだ保存していなければ保存する
     * @param save_point 現在のセーブポイント
     * @param current    現在の状態スナップショット
     * @return 新規エントリを積んだら true（呼び出し側は dirty マーク等を行う）
     */
    bool save_if_needed(int save_point, const State& current) {
        if (trail_.empty() || trail_.back().first != save_point) {
            trail_.push_back({save_point, current});
            return true;
        }
        return false;
    }

    /**
     * @brief save_point より新しいエントリを巻き戻す
     *
     * 新しい順に各エントリの状態を @p restore に渡す。最後（最古）に渡された状態が
     * save_point 時点の状態となる。
     *
     * @param save_point 巻き戻し先のセーブポイント
     * @param restore    State を受け取り、メンバ変数へ復元するコールバック
     */
    template <typename Restore>
    void rewind_to(int save_point, Restore restore) {
        while (!trail_.empty() && trail_.back().first > save_point) {
            restore(trail_.back().second);
            trail_.pop_back();
        }
    }

    /// trail を空にする（探索開始前の再初期化用）
    void clear() { trail_.clear(); }

    /// エントリが1つもないか
    bool empty() const { return trail_.empty(); }

private:
    std::vector<std::pair<int, State>> trail_;
};

}  // namespace sabori_csp

#endif  // SABORI_CSP_CONSTRAINT_TRAIL_HPP
