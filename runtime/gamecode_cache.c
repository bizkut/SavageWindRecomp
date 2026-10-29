#include "gamecode_cache.h"

#include <elf.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wire format uses little-endian integers, never native structures or addresses. */
enum { MANIFEST_SIZE = 704, FIXUP_SIZE = 48, IDENTITY_OFFSET = 168,
       IMAGE_HASH = 608, FIXUP_HASH = 640, MANIFEST_HASH = 672 };
static const unsigned char magic[8] = {'S', 'W', 'G', 'C', 'A', 'C', 'H', '1'};

static void put64(unsigned char *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (i * 8));
}

static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static int fail(char *error, size_t size, const char *fmt, ...)
{
    if (error && size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(error, size, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int digest(const void *data, size_t size, unsigned char out[32])
{
    return mbedtls_sha256_ret(data, size, out, 0) == 0;
}

static void encode_identity(unsigned char *p, const GameCodeIdentity *id)
{
    memcpy(p, id->module_build_ids, 96);
    memcpy(p + 96, id->module_sha256, 96);
    memcpy(p + 192, id->generator_version, 32);
    memcpy(p + 224, id->llvm_version, 128);
    memcpy(p + 352, id->compile_flags_sha256, 32);
    memcpy(p + 384, id->export_abi_sha256, 32);
}

static void encode_fixup(unsigned char *p, const GameLinkFixup *f)
{
    memset(p, 0, FIXUP_SIZE);
    put64(p, f->offset);
    put64(p + 8, (uint64_t)f->addend);
    put64(p + 16, f->slot_offset);
    put64(p + 24, f->target.value);
    put64(p + 32, (uint64_t)f->target.kind | ((uint64_t)f->type << 32));
    put64(p + 40, f->place_view);
}

static int target_valid(GameLinkTarget target, size_t image_size, size_t export_count, int layout)
{
    switch (target.kind) {
    case GAME_LINK_TARGET_NONE:
        return layout && !target.value;
    case GAME_LINK_TARGET_RW:
    case GAME_LINK_TARGET_RX:
        return target.value <= image_size;
    case GAME_LINK_TARGET_EXPORT:
        return !layout && target.value < export_count;
    case GAME_LINK_TARGET_ABSOLUTE:
        return !layout;
    default:
        return 0;
    }
}

static int valid_fixup(const GameLinkFixup *f, const GameLinkImage *image, size_t export_count)
{
    size_t width;
    size_t slot_width;
    size_t image_size = image->size;
    switch (f->type) {
    case R_X86_64_64:
        width = 8;
        slot_width = 0;
        break;
    case R_X86_64_PC32:
        width = 4;
        slot_width = 0;
        break;
    case R_X86_64_PLT32:
        width = 4;
        slot_width = 16;
        break;
    case 41: /* R_X86_64_GOTPCRELX */
    case 42: /* R_X86_64_REX_GOTPCRELX */
        width = 4;
        slot_width = 8;
        break;
    default:
        return 0;
    }
    if (f->place_view != GAME_LINK_TARGET_RW && f->place_view != GAME_LINK_TARGET_RX)
        return 0;
    if (f->offset > image->got_offset || width > image->got_offset - f->offset ||
        !target_valid(f->target, image_size, export_count, 0))
        return 0;
    if (!slot_width)
        return f->slot_offset == UINT64_MAX;
    size_t base = slot_width == 8 ? image->got_offset : image->thunk_offset;
    size_t count = slot_width == 8 ? image->got_count : image->thunk_capacity;
    return f->slot_offset >= base && f->slot_offset - base < count * slot_width &&
           (f->slot_offset - base) % slot_width == 0;
}

int gamecode_cache_save(const GameLinkImage *image, const GameCodeIdentity *identity,
                        const GameCodeCacheWriter *writer, char *error, size_t error_size)
{
    unsigned char manifest[MANIFEST_SIZE] = {0};
    unsigned char *stream = NULL;
    size_t stream_size;
    int ok = 0;
    if (error && error_size)
        error[0] = 0;
    if (!image || !identity || !writer || !writer->write || !image->canonical ||
        (image->fixup_count && !image->fixups) || !memchr(identity->llvm_version, 0, 128) ||
        image->fixup_count > SIZE_MAX / FIXUP_SIZE)
        return fail(error, error_size, "invalid cache save arguments");
    stream_size = image->fixup_count * FIXUP_SIZE;
    stream = malloc(stream_size ? stream_size : 1);
    if (!stream)
        return fail(error, error_size, "cache allocation failed");
    for (size_t i = 0; i < image->fixup_count; i++)
        encode_fixup(stream + i * FIXUP_SIZE, &image->fixups[i]);
    memcpy(manifest, magic, sizeof magic);
    put64(manifest + 8, 1);
    put64(manifest + 16, image->size);
    put64(manifest + 24, image->alignment);
    put64(manifest + 32, image->text_size);
    put64(manifest + 40, image->rodata_size);
    put64(manifest + 48, image->data_size);
    put64(manifest + 56, image->bss_size);
    put64(manifest + 64, image->got_count);
    put64(manifest + 72, image->thunk_count);
    put64(manifest + 80, image->fixup_count);
    for (size_t i = 0; i < 5; i++) {
        put64(manifest + 88 + i * 16, image->layout[i].value);
        put64(manifest + 96 + i * 16, image->layout[i].kind);
    }
    encode_identity(manifest + IDENTITY_OFFSET, identity);
    put64(manifest + 584, image->got_offset);
    put64(manifest + 592, image->thunk_offset);
    put64(manifest + 600, image->thunk_capacity);
    if (!digest(image->canonical, image->size, manifest + IMAGE_HASH) ||
        !digest(stream, stream_size, manifest + FIXUP_HASH) ||
        !digest(manifest, MANIFEST_HASH, manifest + MANIFEST_HASH)) {
        fail(error, error_size, "cache digest failed");
        goto done;
    }
    if (image->size > UINT64_MAX - MANIFEST_SIZE || stream_size > UINT64_MAX - MANIFEST_SIZE - image->size ||
        !writer->write(writer->context, 0, manifest, sizeof manifest) ||
        !writer->write(writer->context, MANIFEST_SIZE, image->canonical, image->size) ||
        !writer->write(writer->context, MANIFEST_SIZE + image->size, stream, stream_size)) {
        fail(error, error_size, "cache write failed");
        goto done;
    }
    ok = 1;
done:
    free(stream);
    return ok;
}

int gamecode_cache_load(const GameCodeIdentity *identity, const GameCodeCacheReader *reader,
                        const GameLinkAllocator *allocator, const GameExport *exports, size_t export_count,
                        GameLinkImage *image, char *error, size_t error_size)
{
    unsigned char manifest[MANIFEST_SIZE], expected_id[416], hash[32];
    unsigned char *stream = NULL;
    size_t stream_size;
    int ok = 0;
    if (error && error_size)
        error[0] = 0;
    if (!identity || !reader || !reader->read || !allocator || !allocator->allocate || !allocator->release ||
        !image || (export_count && !exports) || !memchr(identity->llvm_version, 0, 128))
        return fail(error, error_size, "invalid cache load arguments");
    memset(image, 0, sizeof(*image));
    if (reader->size < MANIFEST_SIZE || !reader->read(reader->context, 0, manifest, sizeof manifest) ||
        memcmp(manifest, magic, sizeof magic) || get64(manifest + 8) != 1 ||
        !digest(manifest, MANIFEST_HASH, hash) || memcmp(hash, manifest + MANIFEST_HASH, 32))
        return fail(error, error_size, "invalid cache manifest");
    encode_identity(expected_id, identity);
    if (memcmp(expected_id, manifest + IDENTITY_OFFSET, sizeof expected_id))
        return fail(error, error_size, "cache identity mismatch");
    uint64_t size64 = get64(manifest + 16), align64 = get64(manifest + 24), count64 = get64(manifest + 80);
    if (!size64 || size64 > SIZE_MAX || !align64 || align64 > SIZE_MAX || (align64 & (align64 - 1)) ||
        count64 > SIZE_MAX / FIXUP_SIZE || size64 > reader->size - MANIFEST_SIZE ||
        count64 != (reader->size - MANIFEST_SIZE - size64) / FIXUP_SIZE ||
        (reader->size - MANIFEST_SIZE - size64) % FIXUP_SIZE || count64 > SIZE_MAX / sizeof(GameLinkFixup))
        return fail(error, error_size, "invalid cache lengths");
    image->size = (size_t)size64;
    image->alignment = (size_t)align64;
    image->fixup_count = (size_t)count64;
    stream_size = image->fixup_count * FIXUP_SIZE;
    uint64_t sum = 0;
    size_t *sections[] = {&image->text_size, &image->rodata_size, &image->data_size, &image->bss_size};
    for (size_t i = 0; i < 4; i++) {
        uint64_t n = get64(manifest + 32 + i * 8);
        if (n > image->size - sum)
            goto invalid;
        *sections[i] = (size_t)n;
        sum += n;
    }
    if (get64(manifest + 64) > SIZE_MAX || get64(manifest + 72) > SIZE_MAX)
        goto invalid;
    image->got_count = (size_t)get64(manifest + 64);
    image->thunk_count = (size_t)get64(manifest + 72);
    uint64_t got = get64(manifest + 584), thunk = get64(manifest + 592), capacity = get64(manifest + 600);
    if (got < sum || got > image->size || image->got_count > (image->size - got) / 8 ||
        thunk > image->size || capacity > (image->size - thunk) / 16 ||
        got + image->got_count * 8 > thunk || image->thunk_count > capacity)
        goto invalid;
    image->got_offset = (size_t)got;
    image->thunk_offset = (size_t)thunk;
    image->thunk_capacity = (size_t)capacity;
    for (size_t i = 0; i < 5; i++) {
        uint64_t kind = get64(manifest + 96 + i * 16);
        if (kind > UINT32_MAX)
            goto invalid;
        image->layout[i].kind = (uint32_t)kind;
        image->layout[i].value = get64(manifest + 88 + i * 16);
        if (!target_valid(image->layout[i], image->size, export_count, 1))
            goto invalid;
    }
    image->canonical = malloc(image->size);
    image->fixups = calloc(image->fixup_count ? image->fixup_count : 1, sizeof(*image->fixups));
    stream = malloc(stream_size ? stream_size : 1);
    if (!image->canonical || !image->fixups || !stream) {
        fail(error, error_size, "cache allocation failed");
        goto done;
    }
    if (!reader->read(reader->context, MANIFEST_SIZE, image->canonical, image->size) ||
        !reader->read(reader->context, MANIFEST_SIZE + image->size, stream, stream_size) ||
        !digest(image->canonical, image->size, hash) || memcmp(hash, manifest + IMAGE_HASH, 32) ||
        !digest(stream, stream_size, hash) || memcmp(hash, manifest + FIXUP_HASH, 32))
        goto invalid;
    for (size_t i = 0; i < image->fixup_count; i++) {
        const unsigned char *p = stream + i * FIXUP_SIZE;
        GameLinkFixup *f = &image->fixups[i];
        uint64_t kind_type = get64(p + 32), view = get64(p + 40);
        f->offset = get64(p);
        f->addend = (int64_t)get64(p + 8);
        f->slot_offset = get64(p + 16);
        f->target.value = get64(p + 24);
        f->target.kind = (uint32_t)kind_type;
        f->type = (uint32_t)(kind_type >> 32);
        if (view > UINT32_MAX)
            goto invalid;
        f->place_view = (uint32_t)view;
        if (!valid_fixup(f, image, export_count))
            goto invalid;
    }
    if (!allocator->allocate(allocator->context, image->size, image->alignment, &image->rw, &image->rx) || !image->rw) {
        fail(error, error_size, "cache image allocation failed");
        goto done;
    }
    if (((uintptr_t)image->rw & (image->alignment - 1)) || (image->rx & (image->alignment - 1)) ||
        image->size > UINTPTR_MAX - (uintptr_t)image->rw || image->size > UINTPTR_MAX - image->rx ||
        !gamecode_link_apply_fixups(image, image->rw, image->rx, exports, export_count, error, error_size))
        goto done;
    void **outputs[] = {&image->game_modules, &image->game_module_count, &image->game_version,
                        &image->game_sites, &image->game_site_count};
    for (size_t i = 0; i < 5; i++) {
        GameLinkTarget t = image->layout[i];
        if (t.kind != GAME_LINK_TARGET_NONE)
            *outputs[i] = (void *)((t.kind == GAME_LINK_TARGET_RW ? (uintptr_t)image->rw : image->rx) + t.value);
    }
    ok = 1;
    goto done;
invalid:
    fail(error, error_size, "invalid cache contents");
done:
    free(stream);
    if (!ok) {
        if (image->rw)
            allocator->release(allocator->context, image->rw, image->size);
        gamecode_link_free_metadata(image);
        memset(image, 0, sizeof(*image));
    }
    return ok;
}
