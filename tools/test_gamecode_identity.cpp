// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the production object-key function and the linked-image identity together.
#include "../runtime/gamecode_compile.cpp"
#include "../runtime/gamecode_cache.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

static int write_cache(void *context, uint64_t offset, const void *data, size_t size) {
    auto &bytes = *static_cast<std::vector<unsigned char> *>(context);
    if (offset > SIZE_MAX || size > SIZE_MAX - static_cast<size_t>(offset)) return 0;
    size_t end = static_cast<size_t>(offset) + size;
    if (end > bytes.size()) bytes.resize(end);
    std::memcpy(bytes.data() + offset, data, size);
    return 1;
}
static int read_cache(void *context, uint64_t offset, void *data, size_t size) {
    auto &bytes = *static_cast<std::vector<unsigned char> *>(context);
    if (offset > bytes.size() || size > bytes.size() - static_cast<size_t>(offset)) return 0;
    std::memcpy(data, bytes.data() + offset, size);
    return 1;
}
static int allocate(void *, size_t size, size_t alignment, void **rw, uintptr_t *rx) {
    if (posix_memalign(rw, alignment, size)) return 0;
    *rx = reinterpret_cast<uintptr_t>(*rw);
    return 1;
}
static void release(void *, void *rw, size_t) { std::free(rw); }

int main() {
    GameCodeIdentity a{};
    a.linker_version = GAMECODE_LINKER_VERSION;
    a.cache_format_version = GAMECODE_CACHE_FORMAT_VERSION;
    a.llvm_build_sha256[0] = 1;
    a.export_abi_sha256[0] = 2;
    GameCodeIdentity b = a;
    b.export_abi_sha256[0] ^= 1;
    Job job{"test.c", "int test(void) { return 1; }"};
    Digest headers{};
    Digest compiler_a, compiler_b;
    std::copy(a.llvm_build_sha256, a.llvm_build_sha256 + 32, compiler_a.begin());
    std::copy(b.llvm_build_sha256, b.llvm_build_sha256 + 32, compiler_b.begin());
    Digest key = cache_key(job, headers, compiler_a);
    assert(key == cache_key(job, headers, compiler_b));
    compiler_b[0] ^= 1;
    assert(key != cache_key(job, headers, compiler_b));
    headers[0] ^= 1;
    assert(key != cache_key(job, headers, compiler_a));
    headers[0] ^= 1;
    job.source += " ";
    assert(key != cache_key(job, headers, compiler_a));
    job.source.pop_back();
    job.name = "other.c";
    assert(key != cache_key(job, headers, compiler_a));

    unsigned char canonical[16]{};
    GameLinkImage image{};
    image.size = sizeof canonical;
    image.text_size = sizeof canonical;
    image.alignment = 16;
    image.got_offset = sizeof canonical;
    image.thunk_offset = sizeof canonical;
    image.canonical = canonical;
    std::vector<unsigned char> bytes;
    GameCodeCacheWriter writer{&bytes, write_cache};
    char error[256];
    assert(gamecode_cache_save(&image, &a, &writer, error, sizeof error));
    GameCodeCacheReader reader{&bytes, bytes.size(), read_cache};
    GameLinkAllocator allocator{nullptr, allocate, release};
    GameLinkImage loaded{};
    assert(!gamecode_cache_load(&b, &reader, &allocator, nullptr, 0, &loaded, error, sizeof error));
    assert(!std::strcmp(error, "cache identity mismatch"));
    assert(gamecode_cache_load(&a, &reader, &allocator, nullptr, 0, &loaded, error, sizeof error));
    gamecode_link_free_metadata(&loaded);
    release(nullptr, loaded.rw, loaded.size);
    std::puts("object keys ignore export ABI; linked-image identity rejects export ABI changes");
}
