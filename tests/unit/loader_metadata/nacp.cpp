// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "yuzu_common/string_util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
bool Valid(std::vector<u8> bytes) {
    FileSys::NACP nacp(std::make_shared<FileSys::VectorVfsFile>(std::move(bytes)));
    return nacp.IsValid();
}
int main(int argc, char** argv) {
    for (const auto* text : {"", "ASCII", "Édition française", "ゼルダの伝説", "🌍"})
        Check(Common::IsValidUTF8(text), "valid UTF-8");
    for (const auto* text : {"\x80", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80",
                            "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe2\x82"})
        Check(!Common::IsValidUTF8(text), "malformed UTF-8 rejected");
    std::vector<u8> bytes(sizeof(FileSys::RawNACP));
    std::memcpy(bytes.data(), "Édition française", sizeof("Édition française"));
    Check(Valid(bytes), "localized NACP accepted");
    auto malformed = bytes;
    malformed[0] = 0xff;
    Check(!Valid(malformed), "malformed NACP rejected");
    malformed = bytes;
    std::memset(malformed.data(), 'x', 0x200);
    Check(!Valid(malformed), "unterminated NACP rejected");
    bytes.pop_back();
    Check(!Valid(bytes), "truncated NACP rejected");
    std::puts("PASS: UTF-8 and production NACP validation");
    // Optional local reproductions: a valid base NACP followed by a malformed update.
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        Check(input.is_open(), "open local NACP reproduction");
        std::vector<u8> data{std::istreambuf_iterator<char>(input), {}};
        const auto valid = Valid(std::move(data));
        Check(valid == (i == 1), "base accepted and malformed update rejected");
        std::printf("PASS: local NACP %s is %s\n", argv[i], valid ? "valid" : "invalid");
    }
}
