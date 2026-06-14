#ifndef SABORI_CSP_SPARSE_SET_POOL_HPP
#define SABORI_CSP_SPARSE_SET_POOL_HPP

#include "sabori_csp/domain.hpp"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sabori_csp {

/**
 * @brief 可逆 Sparse Set による値プール
 *
 * all_different / all_different_except_0 等の制約が「まだ使える値の集合」を
 * O(1) で管理するための共通実装。dense 配列と値→位置の sparse 写像を保持し、
 * remove() はスワップで O(1)。アクティブ数 size() のみで membership が完全に
 * 決まる（reversible sparse set）ため、バックトラックは active_count() を保存・
 * 復元するだけでよい。
 *
 * sparse 写像は値域が密なら offset 付きフラット配列、疎で広いなら
 * unordered_map を自動選択する。
 */
class SparseSetPool {
public:
    using value_type = Domain::value_type;
    static constexpr size_t npos = SIZE_MAX;

    /**
     * @brief 値集合からプールを構築する（全値をアクティブにする）
     * @param values 重複のない値の配列（昇順である必要はないが、重複は不可）
     */
    void assign(std::vector<value_type> values) {
        dense_ = std::move(values);
        n_ = dense_.size();

        // 値域から sparse 表現を決定
        use_flat_ = false;
        flat_.clear();
        map_.clear();
        offset_ = 0;

        if (!dense_.empty()) {
            value_type lo = dense_[0];
            value_type hi = dense_[0];
            for (value_type v : dense_) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            // span はオーバーフローを避けて 64bit で計算
            uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) -
                                                  static_cast<int64_t>(lo)) + 1;
            uint64_t limit = std::max<uint64_t>(1024, 4ull * dense_.size());
            if (span <= limit) {
                use_flat_ = true;
                offset_ = lo;
                flat_.assign(static_cast<size_t>(span), npos);
            }
        }

        for (size_t i = 0; i < dense_.size(); ++i) {
            set_index(dense_[i], i);
        }
    }

    /**
     * @brief 全値を再びアクティブにし、sparse 写像を現在の dense 順に同期する
     *
     * 探索開始前 (prepare_propagation) に呼ぶ。前回探索でスワップ済みの dense 配列
     * 順序をそのまま使い、位置写像のみ貼り直す。
     */
    void reset_all_active() {
        n_ = dense_.size();
        for (size_t i = 0; i < dense_.size(); ++i) {
            set_index(dense_[i], i);
        }
    }

    /// 値 v が現在アクティブ（プール内）かどうか
    bool contains(value_type v) const {
        size_t p = index_of(v);
        return p != npos && p < n_;
    }

    /// 値 v がプールの登録値でありながら既に消費（非アクティブ）されているか
    bool consumed(value_type v) const {
        size_t p = index_of(v);
        return p != npos && p >= n_;
    }

    /**
     * @brief 値 v をプールから取り除く（スワップ削除、O(1)）
     * @return v がアクティブで取り除けたら true、既に非アクティブ/未知なら false
     */
    bool remove(value_type v) {
        size_t idx = index_of(v);
        if (idx == npos || idx >= n_) {
            return false;
        }
        size_t last = n_ - 1;
        value_type last_val = dense_[last];
        dense_[idx] = last_val;
        dense_[last] = v;
        set_index(last_val, idx);
        set_index(v, last);
        --n_;
        return true;
    }

    /// アクティブな値の数
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }

    /// 全容量（プールに登録された値の総数）
    size_t capacity() const { return dense_.size(); }

    /// 登録された全値（アクティブ・非アクティブ問わず現在の dense 順）
    const std::vector<value_type>& values() const { return dense_; }

    /// アクティブな i 番目の値（i in [0, size())）
    value_type value_at(size_t i) const { return dense_[i]; }

    // ===== Trail サポート =====
    /// アクティブ数のスナップショット（バックトラック時に restore する値）
    size_t active_count() const { return n_; }
    /// アクティブ数を復元する（reversible sparse set: これだけで membership が戻る）
    void restore_active_count(size_t n) { n_ = n; }

private:
    size_t index_of(value_type v) const {
        if (use_flat_) {
            int64_t d = static_cast<int64_t>(v) - static_cast<int64_t>(offset_);
            if (d < 0 || static_cast<uint64_t>(d) >= flat_.size()) return npos;
            return flat_[static_cast<size_t>(d)];
        }
        auto it = map_.find(v);
        return it == map_.end() ? npos : it->second;
    }

    void set_index(value_type v, size_t idx) {
        if (use_flat_) {
            flat_[static_cast<size_t>(static_cast<int64_t>(v) -
                                     static_cast<int64_t>(offset_))] = idx;
        } else {
            map_[v] = idx;
        }
    }

    std::vector<value_type> dense_;  // [0, n_): アクティブ、[n_, capacity): 削除済み
    size_t n_ = 0;

    bool use_flat_ = false;
    value_type offset_ = 0;
    std::vector<size_t> flat_;                       // flat_[v - offset_] = dense 内位置
    std::unordered_map<value_type, size_t> map_;     // フォールバック
};

}  // namespace sabori_csp

#endif  // SABORI_CSP_SPARSE_SET_POOL_HPP
