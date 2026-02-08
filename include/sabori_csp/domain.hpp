/**
 * @file domain.hpp
 * @brief 整数定義域クラス（Sparse Set ベース）
 */
#ifndef SABORI_CSP_DOMAIN_HPP
#define SABORI_CSP_DOMAIN_HPP

#include <vector>
#include <optional>
#include <cstdint>
#include <algorithm>
#include <limits>

namespace sabori_csp {

/**
 * @brief 整数定義域を表すクラス
 *
 * Sparse Set を使用し、O(1) での値の存在確認と削除を実現する。
 * バックトラック時の復元は size (n_) のリセットのみで O(1)。
 * Sparse 配列はフラット vector（offset ベース）で高速ルックアップ。
 */
class Domain {
public:
    using value_type = int64_t;

    /**
     * @brief 空の定義域を作成
     */
    Domain();

    /**
     * @brief 区間定義域を作成
     * @param min 最小値
     * @param max 最大値
     */
    Domain(value_type min, value_type max);

    /**
     * @brief 値リストから定義域を作成
     * @param values 定義域に含める値のリスト
     */
    explicit Domain(std::vector<value_type> values);

    /**
     * @brief 定義域が空かどうか
     */
    bool empty() const { return n_ == 0; }

    /**
     * @brief 定義域のサイズを取得
     */
    size_t size() const { return n_; }

    /**
     * @brief 最小値を取得
     */
    std::optional<value_type> min() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(min_); }

    /**
     * @brief 最大値を取得
     */
    std::optional<value_type> max() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(max_); }

    /**
     * @brief 値が定義域に含まれるか
     */
    bool contains(value_type value) const;

    /**
     * @brief 値を削除
     * @return 値が削除されたらtrue
     */
    bool remove(value_type value);

    /**
     * @brief threshold 未満の値を一括削除
     * @return ドメインが空にならなければ true
     */
    bool remove_below(value_type threshold);

    /**
     * @brief threshold 超の値を一括削除
     * @return ドメインが空にならなければ true
     */
    bool remove_above(value_type threshold);

    /**
     * @brief 指定値に固定
     * @return 成功したらtrue
     */
    bool assign(value_type value);

    /**
     * @brief 全ての有効な値を取得
     */
    std::vector<value_type> values() const;

    /**
     * @brief 単一値に固定されているか
     */
    bool is_singleton() const { return n_ > 0 && min_ == max_; }

    /**
     * @brief Sparse Set のみで値の存在を確認（bounds チェックなし）
     *
     * Model の O(gap) スキャンで使用。
     */
    bool sparse_contains(value_type value) const {
        auto idx_val = static_cast<size_t>(value - offset_);
        if (value < offset_ || idx_val >= sparse_.size()) return false;
        return sparse_[idx_val] < n_;
    }

    /**
     * @brief 初期レンジ（= sparse 配列サイズ = initial_max - initial_min + 1）
     */
    size_t initial_range() const { return sparse_.size(); }

    // ===== Sparse Set 内部アクセス（Model からの操作用） =====

    /**
     * @brief Dense 配列への参照を取得
     */
    std::vector<value_type>& values_ref();
    const std::vector<value_type>& values_ref() const;

    /**
     * @brief 値のインデックスを返す（無ければ SIZE_MAX）
     */
    size_t index_of(value_type val) const;

    /**
     * @brief Dense 配列の有効範囲の先頭ポインタ
     */
    const value_type* begin() const { return values_.data(); }

    /**
     * @brief Dense 配列の有効範囲の末尾ポインタ
     */
    const value_type* end() const { return values_.data() + n_; }

    /**
     * @brief 有効サイズ (n_) を取得
     */
    size_t n() const { return n_; }

    /**
     * @brief 有効サイズを設定（バックトラック用）
     */
    void set_n(size_t n);

    /**
     * @brief 最小値をキャッシュに設定
     */
    void set_min_cache(value_type min);

    /**
     * @brief 最大値をキャッシュに設定
     */
    void set_max_cache(value_type max);

    /**
     * @brief Sparse Set 内でスワップ
     */
    void swap_at(size_t i, size_t j);

    /**
     * @brief 現在の有効な値から min/max を再計算
     */
    void update_bounds();

private:
    std::vector<value_type> values_;  // Dense 配列
    std::vector<size_t> sparse_;      // フラット sparse 配列（sparse_[val - offset_] = index）
    value_type offset_;               // = 初期 min 値
    size_t n_;                        // 有効な値の数
    value_type min_;                  // キャッシュ
    value_type max_;                  // キャッシュ
};

} // namespace sabori_csp

#endif // SABORI_CSP_DOMAIN_HPP
