// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/json.h"
#include <string_view>
#include <set>
#include <map>
#include <algorithm>
namespace VideoCommon::ShaderProfile
{
// Startup-only failure backoff. The caller scopes this ledger to the GPU,
// driver and renderer build; each key also identifies its verified compilation
// inputs. Runtime remains free to
// compile a deferred pipeline normally. Unknown or malformed state fails open.
class NativeWarmupFailures {
public:
    static constexpr size_t MaxEntries = 16384;
    explicit NativeWarmupFailures(std::string scope_) : scope(std::move(scope_)) {}

    void Load(const JsonValue& saved) {
        entries.clear();
        if (!saved.isObject() ||
            (saved["schema"].Type()!=JsonValueType::UnsignedInt && saved["schema"].Type()!=JsonValueType::Int) ||
            saved["schema"].asUInt64() != 1 ||
            saved["scope"].asString() != scope || !saved["entries"].isArray() ||
            saved["entries"].size() > MaxEntries) return;
        std::map<std::string, Entry> loaded;
        for (const auto& value : saved["entries"]) {
            const auto id = value["id"].asString();
            const auto count = value["failures"].asUInt64();
            const auto remaining = value["remaining"].asUInt64();
            const auto integer=[](const JsonValue& n) {
                return n.Type()==JsonValueType::UnsignedInt ||
                    (n.Type()==JsonValueType::Int && n.asInt64()>=0);
            };
            if (id.size()!=64 || !std::ranges::all_of(id, [](char c) {
                    return (c>='0' && c<='9') || (c>='a' && c<='f');
                }) || !integer(value["failures"]) || !integer(value["remaining"]) ||
                count==0 || count>4 || remaining>8 ||
                !loaded.emplace(id,Entry{static_cast<unsigned>(count),static_cast<unsigned>(remaining)}).second)
                return;
        }
        entries=std::move(loaded);
    }
    // Called once per unique, safe startup key, before workers access the ledger.
    bool Defer(const std::string& id) {
        const auto it=entries.find(id);
        if (it==entries.end() || it->second.remaining==0) return false;
        --it->second.remaining;
        return true;
    }
    // Caller serializes worker completions with its startup mutex.
    void Complete(const std::string& id, bool success) {
        if (success) { entries.erase(id); return; }
        if (entries.size()>=MaxEntries && !entries.contains(id)) return;
        auto& entry=entries[id];
        entry.failures=std::min(4U,entry.failures+1);
        entry.remaining=1U<<(entry.failures-1); // Retry after 1, 2, 4, then 8 startups.
    }
    JsonValue Save() const {
        JsonValue result;
        result["schema"]=1U;result["scope"]=scope;
        result["entries"]=JsonValue(JsonValueType::Array);
        for (const auto& [id,entry]:entries) {
            JsonValue value;value["id"]=id;value["failures"]=entry.failures;
            value["remaining"]=entry.remaining;result["entries"].Append(value);
        }
        return result;
    }
private:
    struct Entry { unsigned failures{},remaining{}; };
    std::string scope;
    std::map<std::string,Entry> entries;
};
inline std::string LocalRecordHex(std::string_view id) {
    std::string result;result.reserve(id.size()*2);
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:id){result+=hex[c>>4];result+=hex[c&15];}
    return result;
}
inline std::string LocalRecordBinary(std::string_view hex) {
    if (hex.size()!=64) return {};
    std::string binary;
    const auto nibble=[](char c)->int{return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;};
    for(size_t i=0;i<hex.size();i+=2) {
        const int a=nibble(hex[i]),b=nibble(hex[i+1]);
        if(a<0 || b<0)return {};
        binary+=static_cast<char>((a<<4)|b);
    }
    return binary;
}
// Revisions reuse only recipes whose exact local records are still verified.
// The scope excludes the community file digest, but includes game/GPU projection/ABI.
inline std::map<std::string,std::string> ReusableRecipeRecords(const JsonValue& receipt,
    std::string_view scope, const std::set<std::string>& present) {
    std::map<std::string,std::string> result;
    const auto& records=receipt["recipe_records"];
    if(scope.empty() || receipt["recipe_scope"].asString()!=scope || !records.isObject() || records.size()>65536)return result;
    for(const auto& recipe:records.GetMemberNames()) {
        if(LocalRecordBinary(recipe).empty() || !records[recipe].isString())return {};
        const auto record=LocalRecordBinary(records[recipe].asString());
        if(record.empty())return {};
        if(present.contains(record))result.emplace(recipe,record);
    }
    return result;
}
inline bool IntegratedRecordsPresent(const JsonValue& receipt, std::string_view stamp,
                                      std::string_view selection, const std::set<std::string>& present) {
    const auto integer=[](const JsonValue& value){return value.Type()==JsonValueType::UnsignedInt ||
        (value.Type()==JsonValueType::Int && value.asInt64()>=0);};
    if(!receipt["complete"].isBool() || !receipt["complete"].asBool() || !receipt.isObject() || !integer(receipt["schema"]) || !integer(receipt["record_count"]) ||
       receipt["schema"].asUInt64()!=1 || receipt["stamp"].asString()!=stamp ||
       stamp.empty() || (!selection.empty() && receipt["selection"].asString()!=selection))return false;
    const auto& ids=receipt["record_ids"];
    if(!ids.isArray() || ids.size()>65536 || receipt["record_count"].asUInt64()!=ids.size())return false;
    std::set<std::string> unique;
    for(const auto& value:ids) {
        if(!value.isString() || value.asString().size()!=64)return false;
        const auto hex=value.asString();std::string binary;
        const auto nibble=[](char c)->int{return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;};
        for(size_t i=0;i<hex.size();i+=2){int a=nibble(hex[i]),b=nibble(hex[i+1]);if(a<0 || b<0)return false;binary+=char((a<<4)|b);}
        if(!unique.insert(binary).second || !present.contains(binary))return false;
    }
    return true;
}
inline bool ShouldReconstructProfile(bool identified, bool explicit_cli, bool integrated,
                                      std::string_view selection, std::string_view applied_selection) noexcept {
    if(integrated)return false;
    if(explicit_cli)return true;
    return identified || (!selection.empty() && selection!=applied_selection);
}
inline bool CanPublishImport(bool ready, std::string_view stamp, bool cancelled,
                             size_t built, size_t queued, size_t failures) noexcept
{
    return ready && !stamp.empty() && !cancelled && built == queued && failures == 0;
}
inline bool ReusableImportMarker(const JsonValue & marker, std::string_view stamp)
{
    if (stamp.empty() || !marker.isObject()) return false;
    for (const char * key : {"schema", "native_queued", "native_completed", "native_failures"})
    {
        const auto & value = marker[key];
        if (value.Type() != JsonValueType::UnsignedInt &&
            !(value.Type() == JsonValueType::Int && value.asInt64() >= 0)) return false;
    }
    return marker["schema"].asUInt64() == 2 && marker["stamp"].asString() == stamp &&
           marker["status"].asString() == "ready-compatible-subset" && marker["native_failures"].asUInt64() == 0 &&
           marker["native_completed"].asUInt64() == marker["native_queued"].asUInt64();
}
} // namespace VideoCommon::ShaderProfile
