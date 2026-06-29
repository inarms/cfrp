/*
 * Copyright 2026 inarms
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <zstd.h>
#include <vector>
#include <memory>

namespace cfrp {
namespace common {

class ZstdCompressor {
public:
    ZstdCompressor() {
        cctx_ = ZSTD_createCCtx();
    }

    ~ZstdCompressor() {
        if (cctx_) ZSTD_freeCCtx(cctx_);
    }

    // Non-copyable
    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;

    size_t compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, int compressionLevel) {
        return ZSTD_compressCCtx(cctx_, dst, dstCapacity, src, srcSize, compressionLevel);
    }

    std::vector<uint8_t>& get_compress_buf(size_t size) {
        if (compress_buf_.size() < size) compress_buf_.resize(size);
        return compress_buf_;
    }

private:
    ZSTD_CCtx* cctx_ = nullptr;
    std::vector<uint8_t> compress_buf_;
};

class ZstdDecompressor {
public:
    ZstdDecompressor() {
        dctx_ = ZSTD_createDCtx();
    }

    ~ZstdDecompressor() {
        if (dctx_) ZSTD_freeDCtx(dctx_);
    }

    // Non-copyable
    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;

    size_t decompress(void* dst, size_t dstCapacity, const void* src, size_t compressedSize) {
        return ZSTD_decompressDCtx(dctx_, dst, dstCapacity, src, compressedSize);
    }

    std::vector<uint8_t>& get_decompress_buf(size_t size) {
        if (decompress_buf_.size() < size) decompress_buf_.resize(size);
        return decompress_buf_;
    }

private:
    ZSTD_DCtx* dctx_ = nullptr;
    std::vector<uint8_t> decompress_buf_;
};

} // namespace common
} // namespace cfrp
