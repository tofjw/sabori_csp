/**
 * @file domain.hpp
 * @brief 整数定義域クラス（Sparse Set ベース）
 */
#ifndef SABORI_CSP_DOMAIN_HPP
#define SABORI_CSP_DOMAIN_HPP

#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <algorithm>

namespace sabori_csp {

/**
 * @brief 整数定義域を表すクラス
 *
 * Sparse Set を使用し、O(1) での値の存在確認と削除を実現する。
 * バックトラック時の復元は size (n_) のリセットのみで O(1)。
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
    bool empty() const;

    /**
     * @brief 定義域のサイズを取得
     */
    size_t size() const;

    /**
     * @brief 最小値を取得
     */
    std::optional<value_type> min() const;

    /**
     * @brief 最大値を取得
     */
    std::optional<value_type> max() const;

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
    bool is_singleton() const;

    // ===== Sparse Set 内部アクセス（Model からの操作用） =====

    /**
     * @brief Dense 配列への参照を取得
     */
    std::vector<value_type>& values_ref();
    const std::vector<value_type>& values_ref() const;

    /**
     * @brief Sparse マップへの参照を取得
     */
    std::unordered_map<value_type, size_t>& sparse_ref();
    const std::unordered_map<value_type, size_t>& sparse_ref() const;

    /**
     * @brief 有効サイズ (n_) を取得
     */
    size_t n() const;

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
    std::unordered_map<value_type, size_t> sparse_;  // 値 → インデックス
    size_t n_;  // 有効な値の数
    value_type min_;  // キャッシュ
    value_type max_;  // キャッシュ
};

} // namespace sabori_csp

#endif // SABORI_CSP_DOMAIN_HPP
