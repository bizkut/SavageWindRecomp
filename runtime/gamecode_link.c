#include "gamecode_link.h"

#include <elf.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42
#endif

typedef struct LinkObject {
    const unsigned char *bytes;
    void *aligned_copy;
    size_t size;
    const Elf64_Shdr *sections;
    size_t section_count;
    const Elf64_Sym *symbols;
    size_t symbol_count;
    const char *strings;
    size_t strings_size;
    size_t *offsets, *got, *thunks;
    const char *name;
} LinkObject;

typedef struct Definition {
    const char *name;
    size_t object, symbol;
    unsigned weak;
} Definition;

typedef struct Link {
    LinkObject *objects;
    size_t count;
    Definition *defs;
    size_t def_capacity;
    const GameExport *exports;
    size_t export_count;
    size_t alignment, size, got_base, thunk_base;
    size_t got_reserved, thunk_reserved, thunk_extra, thunk_used;
    unsigned char *rw;
    uintptr_t rx;
    char *error;
    size_t error_size;
} Link;

static int fail(Link *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (l->error && l->error_size)
        vsnprintf(l->error, l->error_size, fmt, ap);
    va_end(ap);
    return 0;
}

static int within(size_t offset, size_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static int align_size(size_t *value, size_t alignment)
{
    if (!alignment)
        alignment = 1;
    if ((alignment & (alignment - 1)) || *value > SIZE_MAX - (alignment - 1))
        return 0;
    *value = (*value + alignment - 1) & ~(alignment - 1);
    return 1;
}

static const char *string_at(const char *s, size_t size, size_t offset)
{
    if (offset >= size || !memchr(s + offset, 0, size - offset))
        return NULL;
    return s + offset;
}

static uint64_t hash_name(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static Definition *definition(Link *l, const char *name)
{
    size_t slot = (size_t)hash_name(name) & (l->def_capacity - 1);
    while (l->defs[slot].name && strcmp(l->defs[slot].name, name))
        slot = (slot + 1) & (l->def_capacity - 1);
    return &l->defs[slot];
}

static const GameExport *find_export(Link *l, const char *name)
{
    size_t lo = 0, hi = l->export_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, l->exports[mid].name);
        if (!cmp)
            return &l->exports[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static int parse_object(Link *l, size_t index, const GameLinkObject *input)
{
    LinkObject *o = &l->objects[index];
    Elf64_Ehdr h;
    const Elf64_Shdr *symtab = NULL;
    size_t i;
    o->bytes = input->data;
    o->size = input->size;
    o->name = input->name ? input->name : "<object>";
    if (!o->bytes || o->size < sizeof h)
        return fail(l, "%s: truncated ELF header", o->name);
    if ((uintptr_t)o->bytes % _Alignof(Elf64_Shdr)) {
        o->aligned_copy = malloc(o->size);
        if (!o->aligned_copy)
            return fail(l, "out of memory");
        memcpy(o->aligned_copy, o->bytes, o->size);
        o->bytes = o->aligned_copy;
    }
    memcpy(&h, o->bytes, sizeof h);
    if (memcmp(h.e_ident, ELFMAG, SELFMAG) || h.e_ident[EI_CLASS] != ELFCLASS64 ||
        h.e_ident[EI_DATA] != ELFDATA2LSB || h.e_ident[EI_VERSION] != EV_CURRENT ||
        h.e_type != ET_REL || h.e_machine != EM_X86_64 || h.e_version != EV_CURRENT ||
        h.e_shentsize != sizeof(Elf64_Shdr) || !h.e_shnum ||
        h.e_shoff % _Alignof(Elf64_Shdr) ||
        !within(h.e_shoff, (size_t)h.e_shnum * sizeof(Elf64_Shdr), o->size))
        return fail(l, "%s: invalid or unsupported ELF64 x86-64 object", o->name);
    o->sections = (const Elf64_Shdr *)(o->bytes + h.e_shoff);
    o->section_count = h.e_shnum;
    if (h.e_shstrndx >= o->section_count)
        return fail(l, "%s: invalid section names", o->name);
    const Elf64_Shdr *names = &o->sections[h.e_shstrndx];
    if (names->sh_type != SHT_STRTAB || !within(names->sh_offset, names->sh_size, o->size))
        return fail(l, "%s: invalid section names", o->name);
    o->offsets = malloc(o->section_count * sizeof(*o->offsets));
    if (!o->offsets)
        return fail(l, "out of memory");
    for (i = 0; i < o->section_count; i++) {
        const Elf64_Shdr *s = &o->sections[i];
        const char *name = string_at((const char *)o->bytes + names->sh_offset, names->sh_size, s->sh_name);
        o->offsets[i] = SIZE_MAX;
        if (!name || (s->sh_type != SHT_NOBITS && !within(s->sh_offset, s->sh_size, o->size)))
            return fail(l, "%s: invalid section %zu", o->name, i);
        if (s->sh_type == SHT_GROUP || (s->sh_flags & SHF_GROUP))
            return fail(l, "%s: COMDAT/group section %s unsupported", o->name, name);
        if ((s->sh_flags & SHF_TLS) || !strncmp(name, ".tdata", 6) || !strncmp(name, ".tbss", 5))
            return fail(l, "%s: TLS section %s unsupported", o->name, name);
        if (s->sh_type == SHT_INIT_ARRAY || !strncmp(name, ".init_array", 11))
            return fail(l, "%s: .init_array unsupported", o->name);
        if ((s->sh_flags & SHF_ALLOC) && s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS &&
            !(s->sh_type == 0x70000001 && !strcmp(name, ".eh_frame")))
            return fail(l, "%s: unsupported allocatable section %s", o->name, name);
        if (s->sh_type == SHT_REL)
            return fail(l, "%s: REL relocations unsupported", o->name);
        if (s->sh_type == SHT_SYMTAB) {
            if (symtab)
                return fail(l, "%s: multiple symbol tables", o->name);
            symtab = s;
        }
    }
    if (!symtab || symtab->sh_link >= o->section_count || symtab->sh_entsize != sizeof(Elf64_Sym) ||
        symtab->sh_size % sizeof(Elf64_Sym) || symtab->sh_offset % _Alignof(Elf64_Sym))
        return fail(l, "%s: invalid symbol table", o->name);
    const Elf64_Shdr *str = &o->sections[symtab->sh_link];
    if (str->sh_type != SHT_STRTAB)
        return fail(l, "%s: invalid symbol strings", o->name);
    o->symbols = (const Elf64_Sym *)(o->bytes + symtab->sh_offset);
    o->symbol_count = symtab->sh_size / sizeof(Elf64_Sym);
    o->strings = (const char *)o->bytes + str->sh_offset;
    o->strings_size = str->sh_size;
    o->got = malloc(o->symbol_count * sizeof(*o->got));
    o->thunks = malloc(o->symbol_count * sizeof(*o->thunks));
    if (o->symbol_count && (!o->got || !o->thunks))
        return fail(l, "out of memory");
    for (i = 0; i < o->symbol_count; i++) {
        const Elf64_Sym *s = &o->symbols[i];
        const char *name = string_at(o->strings, o->strings_size, s->st_name);
        o->got[i] = SIZE_MAX;
        o->thunks[i] = SIZE_MAX;
        if (!name)
            return fail(l, "%s: invalid symbol name", o->name);
        if (s->st_shndx == SHN_COMMON)
            return fail(l, "%s: common symbol %s unsupported", o->name, name);
        if (ELF64_ST_TYPE(s->st_info) == STT_TLS || !strncmp(name, "__emutls_", 9))
            return fail(l, "%s: TLS symbol %s unsupported", o->name, name);
        if (s->st_shndx != SHN_UNDEF && s->st_shndx != SHN_ABS && s->st_shndx >= o->section_count)
            return fail(l, "%s: invalid symbol section for %s", o->name, name);
    }
    return 1;
}

static int classify(const Elf64_Shdr *s)
{
    if (!(s->sh_flags & SHF_ALLOC))
        return -1;
    if (s->sh_type == SHT_NOBITS)
        return 3;
    if (s->sh_flags & SHF_EXECINSTR)
        return 0;
    return (s->sh_flags & SHF_WRITE) ? 2 : 1;
}

static int symbol_address(Link *l, size_t oi, size_t si, uintptr_t *address)
{
    LinkObject *o = &l->objects[oi];
    const Elf64_Sym *s = &o->symbols[si];
    const char *name = o->strings + s->st_name;
    unsigned bind = ELF64_ST_BIND(s->st_info);
    if (bind != STB_LOCAL && *name && (s->st_shndx == SHN_UNDEF || ELF64_ST_VISIBILITY(s->st_other) == STV_DEFAULT)) {
        Definition *d = definition(l, name);
        if (d->name) {
            o = &l->objects[d->object];
            s = &o->symbols[d->symbol];
        } else {
            const GameExport *e = find_export(l, name);
            if (e) {
                *address = (uintptr_t)e->address;
                return 1;
            }
            if (bind == STB_WEAK) {
                *address = 0;
                return 1;
            }
            return fail(l, "%s: unresolved symbol %s", o->name, name);
        }
    }
    if (s->st_shndx == SHN_UNDEF) {
        if (bind == STB_WEAK) {
            *address = 0;
            return 1;
        }
        return fail(l, "%s: unresolved symbol %s", o->name, name);
    }
    if (s->st_shndx == SHN_ABS) {
        *address = s->st_value;
        return 1;
    }
    const Elf64_Shdr *sec = &o->sections[s->st_shndx];
    if (o->offsets[s->st_shndx] == SIZE_MAX || s->st_value > sec->sh_size)
        return fail(l, "%s: symbol %s points outside an allocated section", o->name, name);
    uintptr_t base = (sec->sh_flags & SHF_WRITE) ? (uintptr_t)l->rw : l->rx;
    *address = base + o->offsets[s->st_shndx] + s->st_value;
    return 1;
}

static int relocation_sections(Link *l, int apply)
{
    size_t extra_index = 0;
    for (size_t oi = 0; oi < l->count; oi++) {
        LinkObject *o = &l->objects[oi];
        for (size_t j = 0; j < o->section_count; j++) {
            const Elf64_Shdr *r = &o->sections[j];
            if (r->sh_type != SHT_RELA)
                continue;
            if (r->sh_info >= o->section_count || r->sh_link >= o->section_count ||
                o->sections[r->sh_link].sh_type != SHT_SYMTAB ||
                r->sh_entsize != sizeof(Elf64_Rela) || r->sh_size % sizeof(Elf64_Rela) ||
                r->sh_offset % _Alignof(Elf64_Rela))
                return fail(l, "%s: invalid relocation section", o->name);
            const Elf64_Shdr *target = &o->sections[r->sh_info];
            const Elf64_Rela *rel = (const Elf64_Rela *)(o->bytes + r->sh_offset);
            size_t n = r->sh_size / sizeof(*rel);
            for (size_t k = 0; k < n; k++) {
                size_t si = ELF64_R_SYM(rel[k].r_info);
                unsigned type = ELF64_R_TYPE(rel[k].r_info);
                size_t width = type == R_X86_64_64 ? 8 : 4;
                if (type != R_X86_64_64 && type != R_X86_64_PC32 && type != R_X86_64_PLT32 &&
                    type != R_X86_64_GOTPCRELX && type != R_X86_64_REX_GOTPCRELX)
                    return fail(l, "%s: unsupported relocation type %u", o->name, type);
                if (si >= o->symbol_count || !within(rel[k].r_offset, width, target->sh_size))
                    return fail(l, "%s: invalid relocation", o->name);
                if (o->offsets[r->sh_info] == SIZE_MAX)
                    continue;
                if (!apply) {
                    if ((type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX) &&
                        o->got[si] == SIZE_MAX)
                        o->got[si] = l->got_reserved++;
                    if (type == R_X86_64_PLT32) {
                        if (rel[k].r_addend != -4)
                            l->thunk_extra++;
                        else if (o->thunks[si] == SIZE_MAX)
                            o->thunks[si] = l->thunk_reserved++;
                    }
                    continue;
                }
                size_t extra_slot = 0;
                if (type == R_X86_64_PLT32 && rel[k].r_addend != -4)
                    extra_slot = l->thunk_reserved + extra_index++;
                uintptr_t s;
                if (!symbol_address(l, oi, si, &s))
                    return 0;
                size_t pos = o->offsets[r->sh_info] + rel[k].r_offset;
                uintptr_t p = ((target->sh_flags & SHF_WRITE) ? (uintptr_t)l->rw : l->rx) + pos;
                __int128 value;
                if (type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX) {
                    size_t slot = l->got_base + o->got[si] * 8;
                    memcpy(l->rw + slot, &s, 8);
                    value = (__int128)l->rx + slot + rel[k].r_addend - p;
                } else if (type == R_X86_64_64) {
                    value = (__int128)s + rel[k].r_addend;
                } else {
                    value = (__int128)s + rel[k].r_addend - p;
                    if (type == R_X86_64_PLT32 && (value < INT32_MIN || value > INT32_MAX)) {
                        size_t used_flag = (size_t)1 << (sizeof(size_t) * CHAR_BIT - 1);
                        int extra = rel[k].r_addend != -4;
                        size_t index = extra ? extra_slot : (o->thunks[si] & ~used_flag);
                        size_t slot = l->thunk_base + index * 16;
                        __int128 destination = (__int128)s + rel[k].r_addend + 4;
                        if (destination < 0 || destination > UINTPTR_MAX)
                            return fail(l, "%s: PLT32 thunk destination overflow", o->name);
                        uintptr_t thunk_target = (uintptr_t)destination;
                        unsigned char thunk[16] = {0xff, 0x25, 0, 0, 0, 0};
                        memcpy(thunk + 6, &thunk_target, 8);
                        memcpy(l->rw + slot, thunk, sizeof thunk);
                        value = (__int128)l->rx + slot - 4 - p;
                        if (extra || !(o->thunks[si] & used_flag)) {
                            l->thunk_used++;
                            if (!extra)
                                o->thunks[si] |= used_flag;
                        }
                    }
                }
                if (type == R_X86_64_64) {
                    if (value < 0 || value > UINT64_MAX)
                        return fail(l, "%s: absolute relocation overflow", o->name);
                    uint64_t v = (uint64_t)value;
                    memcpy(l->rw + pos, &v, sizeof v);
                } else {
                    if (value < INT32_MIN || value > INT32_MAX)
                        return fail(l, "%s: PC-relative relocation out of range", o->name);
                    int32_t v = (int32_t)value;
                    memcpy(l->rw + pos, &v, sizeof v);
                }
            }
        }
    }
    return 1;
}

int gamecode_link(const GameLinkObject *objects, size_t count, const GameExport *exports,
                  size_t export_count, const GameLinkAllocator *allocator, GameLinkImage *image,
                  char *error, size_t error_size)
{
    Link l = {0};
    size_t total_symbols = 0, category_start;
    int ok = 0;
    l.error = error;
    l.error_size = error_size;
    l.exports = exports;
    l.export_count = export_count;
    l.count = count;
    l.alignment = 16;
    if (error && error_size)
        error[0] = 0;
    if (!image || !allocator || !allocator->allocate || !allocator->release ||
        (count && !objects) || (export_count && !exports))
        return fail(&l, "invalid linker arguments");
    memset(image, 0, sizeof(*image));
    for (size_t i = 0; i < export_count; i++) {
        if (!exports[i].name || (i && strcmp(exports[i - 1].name, exports[i].name) >= 0))
            return fail(&l, "export table is not strictly sorted");
    }
    l.objects = calloc(count ? count : 1, sizeof(*l.objects));
    if (!l.objects)
        return fail(&l, "out of memory");
    for (size_t i = 0; i < count; i++) {
        if (!parse_object(&l, i, &objects[i]))
            goto done;
        if (l.objects[i].symbol_count > SIZE_MAX - total_symbols) {
            fail(&l, "too many symbols");
            goto done;
        }
        total_symbols += l.objects[i].symbol_count;
    }
    l.def_capacity = 16;
    while (l.def_capacity / 2 < total_symbols) {
        if (l.def_capacity > SIZE_MAX / 2) {
            fail(&l, "too many symbols");
            goto done;
        }
        l.def_capacity *= 2;
    }
    l.defs = calloc(l.def_capacity, sizeof(*l.defs));
    if (!l.defs) {
        fail(&l, "out of memory");
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        LinkObject *o = &l.objects[i];
        for (size_t j = 0; j < o->symbol_count; j++) {
            const Elf64_Sym *s = &o->symbols[j];
            unsigned bind = ELF64_ST_BIND(s->st_info);
            const char *name = o->strings + s->st_name;
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s->st_shndx == SHN_UNDEF || !*name)
                continue;
            Definition *d = definition(&l, name);
            if (d->name && !d->weak && bind == STB_GLOBAL) {
                fail(&l, "%s: duplicate strong definition %s", o->name, name);
                goto done;
            }
            if (!d->name || (d->weak && bind == STB_GLOBAL)) {
                d->name = name;
                d->object = i;
                d->symbol = j;
                d->weak = bind == STB_WEAK;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        LinkObject *o = &l.objects[i];
        for (size_t j = 0; j < o->symbol_count; j++) {
            const Elf64_Sym *s = &o->symbols[j];
            const char *name = o->strings + s->st_name;
            if (s->st_shndx == SHN_UNDEF && ELF64_ST_BIND(s->st_info) == STB_GLOBAL && *name &&
                !definition(&l, name)->name && !find_export(&l, name)) {
                fail(&l, "%s: unresolved symbol %s", o->name, name);
                goto done;
            }
        }
    }
    for (int category = 0; category < 4; category++) {
        category_start = l.size;
        for (size_t i = 0; i < count; i++) {
            LinkObject *o = &l.objects[i];
            for (size_t j = 0; j < o->section_count; j++) {
                const Elf64_Shdr *s = &o->sections[j];
                if (classify(s) != category)
                    continue;
                size_t a = s->sh_addralign ? s->sh_addralign : 1;
                if (!align_size(&l.size, a) || s->sh_size > SIZE_MAX - l.size) {
                    fail(&l, "%s: invalid section alignment or size", o->name);
                    goto done;
                }
                if (a > l.alignment)
                    l.alignment = a;
                o->offsets[j] = l.size;
                l.size += s->sh_size;
            }
        }
        size_t extent = l.size - category_start;
        if (category == 0)
            image->text_size = extent;
        else if (category == 1)
            image->rodata_size = extent;
        else if (category == 2)
            image->data_size = extent;
        else
            image->bss_size = extent;
    }
    if (!relocation_sections(&l, 0))
        goto done;
    if (!align_size(&l.size, 8) || l.got_reserved > (SIZE_MAX - l.size) / 8) {
        fail(&l, "GOT size overflow");
        goto done;
    }
    l.got_base = l.size;
    l.size += l.got_reserved * 8;
    if (!align_size(&l.size, 16) || l.thunk_extra > SIZE_MAX - l.thunk_reserved ||
        l.thunk_reserved + l.thunk_extra > (SIZE_MAX - l.size) / 16) {
        fail(&l, "thunk size overflow");
        goto done;
    }
    l.thunk_base = l.size;
    l.size += (l.thunk_reserved + l.thunk_extra) * 16;
    void *rw = NULL;
    if (!allocator->allocate(allocator->context, l.size, l.alignment, &rw, &l.rx) || !rw) {
        fail(&l, "image allocation failed");
        goto done;
    }
    l.rw = rw;
    if (((uintptr_t)rw & (l.alignment - 1)) || (l.rx & (l.alignment - 1)) ||
        l.size > UINTPTR_MAX - (uintptr_t)rw || l.size > UINTPTR_MAX - l.rx) {
        fail(&l, "image views have invalid alignment or range");
        goto done;
    }
    memset(l.rw, 0, l.size);
    for (size_t i = 0; i < count; i++) {
        LinkObject *o = &l.objects[i];
        for (size_t j = 0; j < o->section_count; j++) {
            const Elf64_Shdr *s = &o->sections[j];
            if (o->offsets[j] != SIZE_MAX && s->sh_type != SHT_NOBITS)
                memcpy(l.rw + o->offsets[j], o->bytes + s->sh_offset, s->sh_size);
        }
    }
    if (!relocation_sections(&l, 1))
        goto done;
    const char *names[] = {"game_modules", "game_module_count", "game_version", "game_sites", "game_site_count"};
    void **outputs[] = {&image->game_modules, &image->game_module_count, &image->game_version,
                       &image->game_sites, &image->game_site_count};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Definition *d = definition(&l, names[i]);
        uintptr_t address;
        if (d->name) {
            if (!symbol_address(&l, d->object, d->symbol, &address))
                goto done;
            *outputs[i] = (void *)address;
        }
    }
    image->rw = l.rw;
    image->rx = l.rx;
    image->size = l.size;
    image->got_count = l.got_reserved;
    image->thunk_count = l.thunk_used;
    ok = 1;
done:
    if (!ok && l.rw)
        allocator->release(allocator->context, l.rw, l.size);
    if (!ok)
        memset(image, 0, sizeof(*image));
    for (size_t i = 0; i < count; i++) {
        free(l.objects[i].offsets);
        free(l.objects[i].got);
        free(l.objects[i].thunks);
        free(l.objects[i].aligned_copy);
    }
    free(l.objects);
    free(l.defs);
    return ok;
}
