#include "nx_tool.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <yuzu_common/fs/fs_util.h>
#include <nxemu/ui/i18n/translation.h>
#include <nx_disk_source.hpp>
#include <nxformat.h>

namespace
{
// Read the same DXCI update partition that nxformat can preserve. Only the
// card and HFS0 headers are needed, even when the firmware is several hundred MB.
uint64_t BundledFirmwareSize(const std::filesystem::path & path, uint64_t fileSize)
{
    std::ifstream input(path, std::ios::binary);
    const auto read = [&](uint64_t offset, void * buffer, size_t size) {
        if (offset > fileSize || size > fileSize - offset) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        return static_cast<bool>(input.read(static_cast<char *>(buffer), size));
    };
    const auto le = [](const uint8_t * bytes, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (8 * i);
        return value;
    };
    std::array<uint8_t, 0x140> card{};
    if (!read(0, card.data(), card.size()) || std::memcmp(card.data() + 0x100, "DXCI", 4) != 0) return 0;
    const uint64_t root = le(card.data() + 0x130, 8);
    std::array<uint8_t, 16> header{};
    if (!read(root, header.data(), header.size()) || std::memcmp(header.data(), "HFS0", 4) != 0) return 0;

    const uint64_t count = le(header.data() + 4, 4);
    const uint64_t namesSize = le(header.data() + 8, 4);
    if (count > 4096 || namesSize > 1024 * 1024) return 0;
    const uint64_t tableSize = count * 64 + namesSize;
    if (tableSize > fileSize - root - header.size()) return 0;
    std::vector<uint8_t> table(tableSize);
    if (!read(root + header.size(), table.data(), table.size())) return 0;
    const uint64_t data = root + header.size() + tableSize;
    for (uint64_t i = 0; i < count; ++i)
    {
        const auto * entry = table.data() + i * 64;
        const auto name = le(entry + 16, 4);
        if (name > namesSize || sizeof("update") > namesSize - name ||
            std::memcmp(table.data() + count * 64 + name, "update", sizeof("update")) != 0) continue;
        const auto offset = le(entry, 8);
        const auto size = le(entry + 8, 8);
        if (offset > fileSize - data || size > fileSize - data - offset || size < header.size() ||
            !read(data + offset, header.data(), header.size()) || std::memcmp(header.data(), "HFS0", 4) != 0)
            return 0;
        const auto entries = le(header.data() + 4, 4);
        const auto strings = le(header.data() + 8, 4);
        return entries != 0 && entries * 64 + strings <= size - header.size() ? size : 0;
    }
    return 0;
}
}

std::vector<std::string> SplitFields(const std::string & line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true)
    {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
        {
            return fields;
        }
        start = tab + 1;
    }
}

bool CanRetryWithoutRecipe(const NxTool::State & state)
{
    if (state.succeeded || state.cancelled)
    {
        return false;
    }
    std::string error = state.error;
    std::transform(error.begin(), error.end(), error.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return error.find("recipe") != std::string::npos || error.find("rebuild") != std::string::npos;
}

std::string ProgressText(const NxTool::State & state)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state.started).count();
    char elapsed[32];
    if (seconds >= 3600)
    {
        std::snprintf(elapsed, sizeof(elapsed), "%lld:%02lld:%02lld", (long long)(seconds / 3600), (long long)(seconds / 60 % 60), (long long)(seconds % 60));
    }
    else
    {
        std::snprintf(elapsed, sizeof(elapsed), "%lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
    }
    if (state.total == 0)
    {
        return Tr("Preparing... (%1)", {elapsed});
    }
    if (state.progress >= state.total)
    {
        return Tr("Finishing... (%1)", {elapsed});
    }
    // It stays below 100 % until the command has returned.
    const uint64_t percent = std::min<uint64_t>(99, state.progress * 100 / state.total);
    return Tr("%1 % done (%2)", {std::to_string(percent), elapsed});
}

NxTool::~NxTool()
{
    Cancel();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool NxTool::Start(std::vector<std::string> arguments, std::vector<ScanSource> knownSources)
{
    if (Busy())
    {
        return false;
    }
    Reset();
    arguments.push_back("--frontend");
    {
        std::lock_guard lock(m_mutex);
        m_state.phase = Phase::Running;
        m_state.started = std::chrono::steady_clock::now();
    }
    m_thread = std::thread(&NxTool::Run, this, std::move(arguments), std::move(knownSources));
    return true;
}

NxTool::State NxTool::Current() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

NxTool::State NxTool::TakeState()
{
    std::lock_guard lock(m_mutex);
    auto records = std::move(m_state.records);
    m_state.records.clear();
    State state = m_state;
    state.records = std::move(records);
    return state;
}

bool NxTool::Busy() const
{
    std::lock_guard lock(m_mutex);
    return m_state.phase == Phase::Running;
}

void NxTool::Cancel()
{
    std::lock_guard lock(m_mutex);
    m_cancel = true;
    if (m_handle != nullptr)
    {
        nxformat_cancel_set(m_handle);
    }
}

void NxTool::Reset()
{
    if (Busy())
    {
        return;
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_state = {};
    m_cancel = false;
}

void NxTool::Line(const char * text)
{
    if (text == nullptr || text[0] != '@')
    {
        return;
    }
    std::vector<std::string> fields = SplitFields(text + 1);
    std::lock_guard lock(m_mutex);
    if (fields[0] == "PROGRESS" && fields.size() > 1)
    {
        m_state.progress = std::strtoull(fields[1].c_str(), nullptr, 10);
    }
    else if (fields[0] == "TOTAL" && fields.size() > 1)
    {
        // Each line adds the bytes of one more step.
        m_state.total += std::strtoull(fields[1].c_str(), nullptr, 10);
    }
    else
    {
        m_state.records.push_back(std::move(fields));
    }
}

void NxTool::Run(std::vector<std::string> arguments, std::vector<ScanSource> knownSources)
{
    std::vector<const char *> argv;
    argv.reserve(arguments.size());
    for (const std::string & argument : arguments)
    {
        argv.push_back(argument.c_str());
    }
    nxformat_cancel * handle = nxformat_cancel_new();
    {
        std::lock_guard lock(m_mutex);
        m_handle = handle;
        if (m_cancel)
        {
            nxformat_cancel_set(handle);
        }
    }
    const nxformat_line_fn onLine = [](void * user, int isError, const char * line) {
        if (isError == 0)
        {
            static_cast<NxTool *>(user)->Line(line);
        }
    };
    char error[1024] = {};
    int code;
    const bool scanGame = arguments.front() == "scan-game";
    if (scanGame || arguments.front() == "scan")
    {
        // Scan metadata per package and compare payloads only for likely duplicates.
        using Records = std::vector<std::vector<std::string>>;
        const auto collect = [](void * user, int isError, const char * line) {
            if (isError == 0 && line != nullptr && line[0] == '@')
            {
                static_cast<Records *>(user)->push_back(SplitFields(line + 1));
            }
        };
        std::set<std::filesystem::path> seen;
        const auto scan = [&](const std::filesystem::path & path) -> Records {
            if (!seen.insert(path.lexically_normal()).second)
            {
                return {};
            }
            const std::string name = Common::FS::PathToUTF8String(path);
            const char * input[] = {"scan", name.c_str(), "--frontend"};
            Records records;
            char failure[1024] = {};
            const int result = nxformat_run(input, 3, collect, &records, handle, failure, sizeof(failure));
            if (result == NXFORMAT_CANCELLED)
            {
                return {};
            }
            if (records.empty())
            {
                records.push_back({"SOURCE", name, "0", "", "Rejected"});
            }
            if (result != NXFORMAT_OK && failure[0] != '\0')
            {
                records.push_back({"ISSUE", name, failure});
            }
            const auto dxci = std::find_if(records.begin(), records.end(), [](const auto & row) {
                return row.size() >= 5 && row[0] == "SOURCE" && row[3] == "Dxci" && row[4] == "Supported";
            });
            if (dxci != records.end())
            {
                const auto bytes = BundledFirmwareSize(path, std::strtoull((*dxci)[2].c_str(), nullptr, 10));
                if (bytes != 0) records.push_back({"FIRMWARE", name, std::to_string(bytes)});
            }
            return records;
        };
        const auto cancelled = [this] {
            std::lock_guard lock(m_mutex);
            return m_cancel;
        };
        const auto sameFile = [&](const std::string & a, const std::string & b) {
            const std::filesystem::path left{Common::FS::ToU8String(a)}, right{Common::FS::ToU8String(b)};
            std::error_code error;
            if (std::filesystem::equivalent(left, right, error)) return true;
            std::ifstream first(left, std::ios::binary), second(right, std::ios::binary);
            if (!first || !second) return false;
            std::vector<char> x(1024 * 1024), y(x.size());
            while (!cancelled())
            {
                first.read(x.data(), x.size());
                second.read(y.data(), y.size());
                const auto count = first.gcount();
                if (count != second.gcount() || !std::equal(x.begin(), x.begin() + count, y.begin())) return false;
                if (first.eof() || second.eof()) return first.eof() && second.eof() && !first.bad() && !second.bad();
                if (!first || !second) return false;
            }
            return false;
        };
        const auto publish = [&](Records records) {
            auto source = std::find_if(records.begin(), records.end(), [](const auto & row) {
                return row.size() >= 5 && row[0] == "SOURCE" && row[4] == "Supported";
            });
            if (source != records.end())
            {
                std::vector<std::string> identities;
                for (const auto & row : records)
                {
                    if (row.size() >= 8 && row[0] == "CONTENT")
                        identities.push_back(row[2] + "/" + row[3] + "/" + row[4] + "/" + row[5]);
                }
                std::sort(identities.begin(), identities.end());
                std::string signature;
                for (const auto & identity : identities) signature += identity + ";";
                if (!signature.empty())
                {
                    const auto duplicate = std::find_if(knownSources.begin(), knownSources.end(), [&](const auto & known) {
                        return known.path != (*source)[1] && known.size == (*source)[2] && known.signature == signature &&
                               sameFile(known.path, (*source)[1]);
                    });
                    (*source).push_back(signature);
                    if (duplicate != knownSources.end()) (*source).push_back(duplicate->path);
                    else knownSources.push_back({(*source)[1], (*source)[2], std::move(signature)});
                }
            }
            std::lock_guard lock(m_mutex);
            m_state.records.insert(m_state.records.end(), std::make_move_iterator(records.begin()), std::make_move_iterator(records.end()));
        };
        const auto isPackage = [](const std::filesystem::directory_entry & entry) {
            auto ext = Common::FS::PathToUTF8String(entry.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::error_code ec;
            return (ext == ".dxci" || ext == ".dnsp" || ext == ".xci" || ext == ".nsp") &&
                   std::filesystem::is_regular_file(entry.symlink_status(ec));
        };
        size_t first = 1;
        if (scanGame && arguments.size() > 2 && !cancelled())
        {
            const std::filesystem::path game{Common::FS::ToU8String(arguments[first++])};
            Records base = scan(game);
            const auto isContent = [](const auto & row) { return row.size() >= 8 && row[0] == "CONTENT"; };
            const auto main = std::find_if(base.begin(), base.end(), [&](const auto & row) { return isContent(row) && row[2] == "base"; });
            const std::string application = main != base.end() ? (*main)[4] : std::string{};
            publish(std::move(base));
            if (!application.empty())
            {
                // Only immediate neighbours, selected by CNMT application ID.
                // A second base (including a combined base/update dump) must
                // not replace or conflict with the explicitly selected game.
                std::error_code ec;
                const auto folder = game.has_parent_path() ? game.parent_path() : std::filesystem::path(".");
                std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
                for (; it != end && !ec && !cancelled(); it.increment(ec))
                {
                    if (!isPackage(*it)) continue;
                    Records records = scan(it->path());
                    const bool supported = std::any_of(records.begin(), records.end(), [](const auto & row) {
                        return row.size() >= 5 && row[0] == "SOURCE" && row[4] == "Supported";
                    });
                    bool foundAddOn = false;
                    bool otherContent = false;
                    for (const auto & row : records)
                    {
                        if (!isContent(row)) continue;
                        if (row[4] == application && (row[2] == "update" || row[2] == "dlc"))
                            foundAddOn = true;
                        else
                            otherContent = true;
                    }
                    if (supported && foundAddOn && !otherContent) publish(std::move(records));
                }
            }
        }
        for (size_t i = first; i + 1 < arguments.size() && !cancelled(); ++i)
        {
            const std::filesystem::path path{Common::FS::ToU8String(arguments[i])};
            std::error_code ec;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(path, ec)))
            {
                publish(scan(path));
                continue;
            }
            std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; it != end && !ec && !cancelled(); it.increment(ec))
            {
                if (it.depth() >= 7)
                {
                    it.disable_recursion_pending();
                }
                if (isPackage(*it))
                {
                    publish(scan(it->path()));
                }
            }
        }
        code = NXFORMAT_OK;
    }
    else
    {
        code = nxformat_run(argv.data(), argv.size(), onLine, this, handle, error, sizeof(error));
        if (code == NXFORMAT_OK && arguments.front() == "contents")
        {
            const auto source = nx::DiskSource::open(std::filesystem::path{Common::FS::ToU8String(arguments[1])});
            if (source)
            {
                const auto archive = nx::Archive::open(**source.value);
                if (archive && !archive.value->applications().empty())
                {
                    // The contents command reports each snapshot's original title.
                    // The library and metadata editor use the application title.
                    std::lock_guard lock(m_mutex);
                    m_state.records.push_back({"APP_TITLE", std::string(archive.value->title(0))});
                }
            }
        }
    }
    std::lock_guard lock(m_mutex);
    m_handle = nullptr;
    nxformat_cancel_free(handle);
    m_state.phase = Phase::Done;
    m_state.cancelled = m_cancel || code == NXFORMAT_CANCELLED;
    m_state.succeeded = code == NXFORMAT_OK && !m_state.cancelled;
    if (!m_state.succeeded && !m_state.cancelled)
    {
        m_state.error = error[0] != '\0' ? error : "the NX tool stopped with an error";
    }
}
