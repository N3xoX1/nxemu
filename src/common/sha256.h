#pragma once

#include <stdint.h>
#include <string>

class SHA256
{
protected:
    const static uint32_t sha256_k[];
    static const unsigned int SHA224_256_BLOCK_SIZE = (512 / 8);

public:
    void init();
    void update(const unsigned char * message, size_t len);
    void final(unsigned char * digest);
    static const unsigned int DIGEST_SIZE = (256 / 8);

protected:
    void transform(const unsigned char * message, size_t block_nb);
    uint64_t m_tot_len;
    unsigned int m_len;
    unsigned char m_block[2 * SHA224_256_BLOCK_SIZE];
    uint32_t m_h[8];
};

std::string sha256(std::string input);