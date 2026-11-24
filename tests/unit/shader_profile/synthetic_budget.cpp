// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_synthetic_budget.h"
#include <iostream>
#include <stdexcept>
using namespace VideoCommon::ShaderProfile;
static void Check(bool ok) { if (!ok) throw std::runtime_error("Synthetic set budget regression"); }
int main() try {
    SyntheticSetBudget budget(2);
    // Recorded recipes always pass and their keys cost nothing later.
    Check(budget.Admit("a", "a1", false));
    Check(budget.Admit("a", "a1", true));
    // The first two new keys of a set pass, a repeat of one of them too, the third does not.
    Check(budget.Admit("a", "a2", true));
    Check(budget.Admit("a", "a3", true));
    Check(budget.Admit("a", "a2", true));
    Check(!budget.Admit("a", "a4", true));
    // A refused key stays refused for synthetic recipes but a recorded one still passes.
    Check(!budget.Admit("a", "a4", true));
    Check(budget.Admit("a", "a4", false));
    // Each set has its own budget.
    Check(budget.Admit("b", "b1", true) && budget.Admit("b", "b2", true) && !budget.Admit("b", "b3", true));
    Check(budget.Rejected() == 3);
    SyntheticSetBudget none(0);
    Check(!none.Admit("a", "a1", true) && none.Admit("a", "a2", false));
    std::cout << "synthetic set budget: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
