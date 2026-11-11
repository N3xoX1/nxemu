#include "common/sha256.h"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string Hex(const std::vector<unsigned char> & data, size_t chunk)
{
    SHA256 hash;
    hash.init();
    for (size_t at = 0; at < data.size(); at += chunk)
        hash.update(data.data() + at, std::min(chunk, data.size() - at));
    unsigned char digest[SHA256::DIGEST_SIZE];
    hash.final(digest);
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char value : digest)
    {
        out += digits[value >> 4];
        out += digits[value & 15];
    }
    return out;
}

void Check(const std::vector<unsigned char> & data, const char * expected)
{
    // One update, odd-sized pieces and whole blocks must all give the same digest.
    for (size_t chunk : {data.size() ? data.size() : size_t{1}, size_t{1}, size_t{7}, size_t{64}, size_t{4099}})
        if (Hex(data, chunk) != expected) throw std::runtime_error("SHA-256 mismatch: " + std::string(expected));
}

std::vector<unsigned char> Bytes(const std::string & text)
{
    return {text.begin(), text.end()};
}
} // namespace

int main()
{
    // FIPS 180-2 vectors, then a pattern spanning many blocks.
    Check({}, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check(Bytes("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Check(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Check(Bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    Check(std::vector<unsigned char>(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::vector<unsigned char> pattern(3 * 1024 * 1024 + 1);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<unsigned char>(i * 31 + 7);
    Check(pattern, "b53d83970dbc592f40694bb16415061e4e9e54594cc88e6450174724dead0eb8");
    pattern.resize(64 * 1024 + 13);
    Check(pattern, "dc7bd6a8f06bb738ef0a2be3be02567a5879bac8aa31ba6359dd087ef4e0346e");
}
