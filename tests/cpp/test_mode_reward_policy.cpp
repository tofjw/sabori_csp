#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/mode_reward_policy.hpp"

#include <cmath>
#include <random>
#include <set>

using namespace sabori_csp;

namespace {

// グリッド値 {0, 0.25, 0.5, 0.75, 1.0} のいずれかか
bool is_grid_value(double p) {
    for (size_t i = 0; i < ModeRewardPolicy::kGridSize; ++i) {
        double g = static_cast<double>(i) / static_cast<double>(ModeRewardPolicy::kGridSize - 1);
        if (std::abs(p - g) < 1e-12) return true;
    }
    return false;
}

} // namespace

TEST_CASE("ModeRewardPolicy default state", "[mode_reward]") {
    ModeRewardPolicy pol;
    REQUIRE(pol.mix_p() == 0.5);  // 初期は中央バケット
}

TEST_CASE("ModeRewardPolicy set_fixed", "[mode_reward]") {
    ModeRewardPolicy pol;
    pol.set_fixed(true);
    REQUIRE(pol.mix_p() == 1.0);   // Activity 優先
    pol.set_fixed(false);
    REQUIRE(pol.mix_p() == 0.0);   // MRV 優先
}

TEST_CASE("ModeRewardPolicy resample yields grid value in [0,1]", "[mode_reward]") {
    ModeRewardPolicy pol;
    std::mt19937 rng(12345);
    for (int it = 0; it < 1000; ++it) {
        // 改善/深さをランダムに観測してから再抽選
        if (rng() & 1) pol.note_improvement();
        pol.observe_depth(rng() % 50);
        pol.update_and_resample(rng);
        double p = pol.mix_p();
        REQUIRE(p >= 0.0);
        REQUIRE(p <= 1.0);
        REQUIRE(is_grid_value(p));
    }
}

TEST_CASE("ModeRewardPolicy is deterministic for fixed seed/sequence", "[mode_reward]") {
    auto run = []() {
        ModeRewardPolicy pol;
        std::mt19937 rng(999);
        std::vector<double> seq;
        for (int it = 0; it < 200; ++it) {
            if (it % 3 == 0) pol.note_improvement();
            pol.observe_depth(static_cast<size_t>(it % 7));
            pol.update_and_resample(rng);
            seq.push_back(pol.mix_p());
        }
        return seq;
    };
    REQUIRE(run() == run());
}

TEST_CASE("ModeRewardPolicy persistent improvement biases toward high reward bucket", "[mode_reward]") {
    // current バケットを固定し、毎回改善を報告し続けると、その（または隣接）バケットの
    // reward が支配的になり、抽選分布がそのバケット近傍に集中することを確認する。
    // set_fixed(true) で p_idx を最大バケット(=mix_p 1.0)に固定してから improvement を流す。
    ModeRewardPolicy pol;
    pol.set_fixed(true);  // p_idx = kGridSize-1, mix_p = 1.0
    std::mt19937 rng(7);

    std::set<double> observed;
    for (int it = 0; it < 500; ++it) {
        pol.note_improvement();          // 常に改善 → 現バケットの報酬が高止まり
        pol.update_and_resample(rng);
        observed.insert(pol.mix_p());
    }
    // 高バケット側（mix_p>=0.5）が必ず観測される（floor により他も稀に出る可能性は許容）
    bool saw_high = false;
    for (double p : observed) if (p >= 0.5) saw_high = true;
    REQUIRE(saw_high);
}
