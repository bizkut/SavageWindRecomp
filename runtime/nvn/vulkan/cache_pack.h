// Shader cache packs (cache_pack.c): the shader precompilation's translations and SPIR-V, in one file per cache folder
// instead of one per shader.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PACK_XLATE, PACK_SPIRV, PACK_KINDS };  // cache/xlate/<translator id>/, cache/spirv/<SPIR-V id>/

// The entry of that key (a translation's id, a SPIR-V source hash): its bytes, malloc'd (free them). false: none.
bool cache_pack_read(int kind, uint64_t key, uint8_t **data, size_t *size);
bool cache_pack_has(int kind, uint64_t key);

// The precompilation (precompile.c): from begin to end, entries stored go into the packs (an interrupted run's are
// dropped: the next begins those packs again); flush: every entry added so far readable.
void cache_pack_begin(void);
bool cache_pack_add(int kind, uint64_t key, const void *data, size_t n);  // false: not now (store a file of its own)
void cache_pack_flush(void);
void cache_pack_end(void);  // flushed, and the indexes written
