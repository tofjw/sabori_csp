#include <catch2/catch_test_macros.hpp>
#include "fzn_parser.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/model_simplifier.hpp"
#include "sabori_csp/solver.hpp"
#include "sabori_csp/parallel_solver.hpp"
#include <cxxabi.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <typeinfo>
#include <unordered_set>
#include <vector>

using namespace sabori_csp;

// ============================================================================
// clone 健全性ゲート（制約ファミリ横断）
//
// test_parallel_clone.cpp が clone するのは AllDifferent / IntLinEq / IntLinLe
// の 3 種だけで、golden master は -j 1 固定なので他の制約の clone() は通らない。
// ここでは golden コーパス（tests/golden/corpus.txt）の全 fzn について
//
//   原本:   Solver::solve_all / solve_optimize
//   clone:  Solver::prepare(master) → master.clone() → solve_*_prepared
//
// を同一シードで走らせ、解の列と決定論的統計が完全一致することを要求する。
// presolve は RNG を消費しないので、clone が制約の状態を正しく複製していれば
// 探索軌道まで一致する。複製漏れ（コピーされないメンバ、master と共有して
// しまう可変状態）は解の欠落・余分・統計のずれとして現れる。
// ============================================================================

namespace {

bool same_stats(const SolverStats& a, const SolverStats& b) {
    return a.max_depth == b.max_depth
        && a.restart_count == b.restart_count
        && a.fail_count == b.fail_count
        && a.nogood_count == b.nogood_count
        && a.nogood_prune_count == b.nogood_prune_count
        && a.nogoods_size == b.nogoods_size
        && a.bisect_count == b.bisect_count
        && a.enumerate_count == b.enumerate_count;
}

std::string demangle(const char* name) {
    int status = 0;
    char* p = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string s = (status == 0 && p) ? p : name;
    std::free(p);
    return s;
}

std::vector<std::string> read_corpus() {
    std::vector<std::string> paths;
    std::ifstream in(std::string(SABORI_SOURCE_DIR) + "/tests/golden/corpus.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) paths.push_back(line);
    }
    return paths;
}

/// fzn → Model（fzn_sabori の既定経路と同じく ModelSimplifier まで適用）。
/// 簡約で確定 UNSAT になった場合は nullptr。
std::unique_ptr<Model> build_model(const fzn::Model& fm) {
    auto model = fm.to_model();
    std::unordered_set<size_t> protected_ids;
    if (fm.solve_decl().kind != fzn::SolveKind::Satisfy &&
        !fm.solve_decl().objective_var.empty()) {
        size_t idx = model->find_variable_index(fm.solve_decl().objective_var);
        if (idx != SIZE_MAX) protected_ids.insert(idx);
    }
    ModelSimplifier simplifier;
    simplifier.simplify(*model, protected_ids, false);
    if (simplifier.is_infeasible()) return nullptr;
    return model;
}

/// 補助変数のうち定数用（__const_ 等）と set_in_reif 分解用（__sir_）は、名前の通し番号が
/// プロセス内 static カウンタ由来で、同じ fzn を 2 回構築すると名前がずれる。解から取り除く
/// （定数は比較する意味がなく、__sir_ の値の食い違いは統計の一致検査で捕まる）。
Solution normalize(const Solution& sol) {
    Solution out;
    for (const auto& [name, v] : sol) {
        if (name.rfind("__const_", 0) == 0 || name.rfind("__idx_const_", 0) == 0 ||
            name.rfind("__res_const_", 0) == 0 || name.rfind("__sir_", 0) == 0) {
            continue;
        }
        out.emplace(name, v);
    }
    return out;
}

struct RunResult {
    std::vector<Solution> solutions;
    SolverStats stats;
    bool searched = false;  ///< 探索まで進んだか（簡約/presolve で UNSAT なら false）
};

RunResult run_original(const fzn::Model& fm) {
    RunResult r;
    auto model = build_model(fm);
    if (!model) return r;
    Solver s;
    auto collect = [&r](const Solution& sol) { r.solutions.push_back(normalize(sol)); return true; };
    const auto& sd = fm.solve_decl();
    if (sd.kind == fzn::SolveKind::Satisfy) {
        s.solve_all(*model, collect);
    } else {
        size_t obj = model->find_variable_index(sd.objective_var);
        s.solve_optimize(*model, obj, sd.kind == fzn::SolveKind::Minimize, collect);
    }
    r.stats = s.stats();
    r.searched = true;
    return r;
}

RunResult run_clone(const fzn::Model& fm, std::set<std::string>& classes) {
    RunResult r;
    auto master = build_model(fm);
    if (!master) return r;
    Solver prep;
    if (!prep.prepare(*master)) return r;  // presolve で UNSAT: 原本も解 0 のはず
    auto model = master->clone();
    for (const auto& c : model->constraints()) {
        if (c) classes.insert(demangle(typeid(*c).name()));
    }

    // clone 後に master 側を別シードで探索させて荒らし、破棄してから clone を解く。
    // clone が master と可変状態を共有していれば軌道がずれ、master を指す参照が
    // 残っていれば解放済みメモリを踏む（ASan ビルドなら確実に落ちる）。
    {
        Solver other;
        WorkerConfig cfg;
        cfg.seed = 7;
        other.apply_worker_config(cfg);
        const auto& sd = fm.solve_decl();
        if (sd.kind == fzn::SolveKind::Satisfy) {
            other.solve_all_prepared(*master, [](const Solution&) { return true; });
        } else {
            size_t obj = master->find_variable_index(sd.objective_var);
            other.solve_optimize_prepared(*master, obj, sd.kind == fzn::SolveKind::Minimize);
        }
        master.reset();
    }

    Solver s;
    s.apply_worker_config(WorkerConfig{});
    auto collect = [&r](const Solution& sol) { r.solutions.push_back(normalize(sol)); return true; };
    const auto& sd = fm.solve_decl();
    if (sd.kind == fzn::SolveKind::Satisfy) {
        s.solve_all_prepared(*model, collect);
    } else {
        size_t obj = model->find_variable_index(sd.objective_var);
        s.solve_optimize_prepared(*model, obj, sd.kind == fzn::SolveKind::Minimize, collect);
    }
    r.stats = s.stats();
    r.searched = true;
    return r;
}

} // namespace

TEST_CASE("clone: golden コーパス全 fzn で原本と解・統計が一致", "[parallel][clone][fzn]") {
    auto corpus = read_corpus();
    REQUIRE(corpus.size() > 200);

    std::set<std::string> classes;
    for (const auto& rel : corpus) {
        CAPTURE(rel);
        auto fm = fzn::parse_file(std::string(SABORI_SOURCE_DIR) + "/" + rel);
        REQUIRE(fm);
        auto a = run_original(*fm);
        auto b = run_clone(*fm, classes);
        CHECK(a.solutions.size() == b.solutions.size());
        CHECK(a.solutions == b.solutions);
        // presolve UNSAT 時の clone 側は探索しないので統計は比較しない
        if (b.searched) {
            CHECK(same_stats(a.stats, b.stats));
        }
    }

    // clone を通った制約クラス（カバレッジの目視用。-s で表示）
    std::cout << "[fzn clone] " << corpus.size() << " fzn, "
              << classes.size() << " constraint classes cloned\n";
    for (const auto& c : classes) std::cout << "  " << c << "\n";
}
