// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace VideoCommon::ShaderProfile {
// The synthetic recipes of one program set come in rank order, the most likely first (a base pack
// writes them so). A device builds only the first two of each set that give it a pipeline it does not
// have yet: on MK8 the later ranks are drawn about half as often (a base pack with model layouts:
// 73 % of the first two used on NVIDIA, 57 % without dynamic vertex input). Recorded recipes and
// synthetic ones seen drawn are always built.
class SyntheticSetBudget {
public:
    static constexpr size_t DefaultPerSet = 2;
    explicit SyntheticSetBudget(size_t per_set = DefaultPerSet) : per_set{per_set} {}
    // set: the programs the recipe draws with; key: the device's pipeline key, set included.
    bool Admit(std::string_view set, std::string_view key, bool synthetic) {
        if (!keys.emplace(key).second || !synthetic) return true;
        auto& used = admitted[std::string(set)];
        if (used >= per_set) {
            keys.erase(std::string(key));
            ++rejected;
            return false;
        }
        ++used;
        return true;
    }
    [[nodiscard]] size_t Rejected() const noexcept { return rejected; }

private:
    size_t per_set, rejected{};
    std::unordered_set<std::string> keys;
    std::unordered_map<std::string, size_t> admitted;
};
} // namespace VideoCommon::ShaderProfile
