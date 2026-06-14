#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_SCHEDULING_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_SCHEDULING_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief Disjunctive (unary resource) 制約
 *
 * 各タスク i は開始時刻 s[i]、実行時間 d[i] を持ち、
 * 任意の2タスク i,j について s[i]+d[i] <= s[j] ∨ s[j]+d[j] <= s[i] を保証。
 * strict=false の場合、d[i]=0 のタスクは他タスクと重複可能。
 */
class DisjunctiveConstraint : public Constraint {
public:
    DisjunctiveConstraint(std::vector<VariablePtr> starts,
                          std::vector<VariablePtr> durations,
                          bool strict);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;

    /**
     * @brief バッチ伝播: 全タスクスキャン + edge-finding を1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

protected:


private:
    size_t n_;          // タスク数
    bool strict_;       // strict disjunctive かどうか
    int offset_;        // 時間軸オフセット (min_start)
    int horizon_;       // 時間軸長 (max_end - min_start)

    std::vector<uint64_t> timeline_;  // ビットマップ (1=占有)

    // Trail
    struct UndoEntry {
        int block_idx;
        uint64_t old_mask;
    };
    std::vector<std::pair<int, UndoEntry>> trail_;

    // Compulsory Part tracking
    std::vector<int> cp_lo_;   // [cp_lo_[i], cp_hi_[i]) = task i の現在 CP 区間
    std::vector<int> cp_hi_;

    struct CpUndoEntry {
        size_t task_idx;
        int old_cp_lo;
        int old_cp_hi;
    };
    std::vector<std::pair<int, CpUndoEntry>> cp_trail_;

    // Task helpers
    bool task_fully_assigned(const Model& model, size_t task) const;
    int task_start(const Model& model, size_t task) const;
    int task_dur(const Model& model, size_t task) const;
    int task_dur_min(const Model& model, size_t task) const;

    // Bit operations
    bool check_conflict(int start, int len) const;
    int count_set_bits(int start, int len) const;
    int count_free_bits(int start, int len) const;
    bool check_conflict_excluding(int start, int len, size_t exclude_task) const;
    int find_first_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const;
    int find_last_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const;
    void set_bits_direct(int start, int len);
    void set_bits(Model& model, int save_point, int start, int len);
    void ensure_dirty_marked(Model& model, int save_point);
    int find_next_zero(int from) const;
    int find_prev_zero(int from) const;

    // Compulsory part
    bool update_compulsory_part(Model& model, int save_point, size_t task);
    bool update_compulsory_part_direct(Model& model, size_t task);

    // Propagation
    bool propagate_bounds(Model& model);
    bool edge_finding(Model& model, bool direct);
};


/**
 * @brief diffn 制約: 非重複矩形配置
 *
 * n 個の矩形 (x[i], y[i]) を原点、(dx[i], dy[i]) をサイズとし、
 * 任意の 2 矩形が重ならないことを保証する。
 * strict=false の場合、サイズ 0 の矩形は他と重複可能。
 */
class DiffnConstraint : public Constraint {
public:
    DiffnConstraint(std::vector<VariablePtr> x, std::vector<VariablePtr> y,
                    std::vector<VariablePtr> dx, std::vector<VariablePtr> dy,
                    bool strict = true);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;

    void rewind_to(int save_point) override;

protected:


private:
    size_t n_;      // 矩形数
    bool strict_;   // strict diffn かどうか

    // var_ids_ レイアウト: [x0..xn-1, y0..yn-1, dx0..dxn-1, dy0..dyn-1]

    bool propagate_pairwise(Model& model);
    bool propagate_pairwise_direct(Model& model);
};


// ============================================================================
// Cumulative constraint
// ============================================================================

/**
 * @brief Cumulative propagator の統計情報
 */
struct CumulativeEngineStats {
    size_t call_count = 0;
    size_t reduction_count = 0;
    size_t contradiction_count = 0;
};


/**
 * @brief Cumulative propagator の抽象基底クラス
 *
 * 各種伝播アルゴリズム（Time-Tabling, Edge-Finding等）を
 * 独立した戦略として実装するためのインターフェース。
 */
class CumulativePropagator {
public:
    virtual ~CumulativePropagator() = default;

    /** @brief エンジン名を返す */
    virtual std::string engine_name() const = 0;

    /** @brief presolve 時の伝播 */
    virtual PresolveResult propagate_presolve(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) = 0;

    /** @brief 探索時の伝播 */
    virtual bool propagate_search(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) = 0;
};


/**
 * @brief Profile-based sweep-line time-tabling propagator
 *
 * Mandatory part を用いた resource profile を構築し、
 * 過負荷チェックと開始時刻のプルーニングを行う。
 */
class TimeTablingPropagator : public CumulativePropagator {
public:
    std::string engine_name() const override { return "TimeTabling"; }

    PresolveResult propagate_presolve(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) override;

    bool propagate_search(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) override;

private:
    /** @brief profile イベント */
    struct Event {
        int64_t time;
        int64_t delta;  // +req or -req
    };

    /**
     * @brief 共通の伝播ロジック
     * @param direct true=presolve（直接ドメイン操作）、false=search（enqueue）
     * @return 矛盾なら false
     */
    bool propagate_impl(Model& model, size_t n,
                        const std::vector<size_t>& var_ids,
                        bool direct, bool& changed);
};


/**
 * @brief Time-Table Edge-Finding propagator (Vilím 2011)
 *
 * TimeTabling の mandatory profile に加え、エネルギー推論で
 * 早期に枝刈りする。Forward pass で EST を引き上げ、
 * backward pass で LST を引き下げる。
 */
class TTEFPropagator : public CumulativePropagator {
public:
    std::string engine_name() const override { return "TTEF"; }

    PresolveResult propagate_presolve(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) override;

    bool propagate_search(
        Model& model, size_t n, const std::vector<size_t>& var_ids,
        CumulativeEngineStats& stats) override;

private:
    struct TaskInfo {
        size_t idx;        // original task index
        int64_t est, lst, ect, lct;
        int64_t dur, req, energy;
    };

    struct ProfileEntry {
        int64_t time;
        int64_t usage;
    };

    std::vector<TaskInfo> tasks_;
    std::vector<ProfileEntry> profile_;
    std::vector<int64_t> prefix_energy_;  // prefix sum for profile integral

    bool propagate_impl(Model& model, size_t n,
                        const std::vector<size_t>& var_ids,
                        bool direct, bool& changed);

    void build_tasks(const Model& model, size_t n,
                     const std::vector<size_t>& var_ids);
    void build_profile();
    int64_t profile_integral(int64_t lo, int64_t hi) const;

    bool forward_pass(Model& model, const std::vector<size_t>& var_ids,
                      int64_t cap_max, bool direct, bool& changed);
    bool backward_pass(Model& model, const std::vector<size_t>& var_ids,
                       int64_t cap_max, bool direct, bool& changed);
};


/**
 * @brief cumulative 制約: リソース容量制約
 *
 * n 個のタスクがあり、タスク i は時刻 start[i] から duration[i] の間、
 * requirement[i] だけリソースを使用する。任意の時点でリソース使用量の
 * 合計が capacity を超えないことを保証する。
 *
 * var_ids_ レイアウト: [s0..sn-1, d0..dn-1, r0..rn-1, capacity]
 */
class CumulativeConstraint : public Constraint {
public:
    CumulativeConstraint(std::vector<VariablePtr> starts,
                         std::vector<VariablePtr> durations,
                         std::vector<VariablePtr> requirements,
                         VariablePtr capacity);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;

    /**
     * @brief バッチ伝播: 全エンジン（timetable + TTEF）を1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

    /** @brief per-engine 統計を取得 */
    const std::vector<CumulativeEngineStats>& engine_stats() const { return engine_stats_; }
    /** @brief エンジン名のリストを取得 */
    std::vector<std::string> engine_names() const;

private:
    size_t n_;  // タスク数

    // Propagator エンジン
    std::vector<std::unique_ptr<CumulativePropagator>> engines_;
    std::vector<CumulativeEngineStats> engine_stats_;

    /** @brief 全エンジンの伝播を実行 */
    bool run_all_engines(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_SCHEDULING_HPP
