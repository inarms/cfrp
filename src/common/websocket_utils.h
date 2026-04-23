#pragma once

#include <string>
#include <vector>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/coding.h>

namespace cfrp {
namespace common {

class WebSocketUtils {
public:
    static std::string GenerateAcceptKey(const std::string& client_key) {
        std::string concat = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        
        Sha sha;
        byte hash[SHA_DIGEST_SIZE];
        wc_InitSha(&sha);
        wc_ShaUpdate(&sha, (const byte*)concat.c_str(), (word32)concat.length());
        wc_ShaFinal(&sha, hash);

        word32 outLen = 0;
        // Calculate required base64 length
        Base64_Encode(hash, SHA_DIGEST_SIZE, NULL, &outLen);
        
        std::vector<byte> out(outLen);
        Base64_Encode(hash, SHA_DIGEST_SIZE, out.data(), &outLen);
        
        return std::string((const char*)out.data(), outLen);
    }
};

} // namespace common
} // namespace cfrp
