#define main gamecode_link_test_main
#include "test_gamecode_link.c"
#undef main

/* Compare symbolic R_X86_64_64 data pointers to the default linked title, not code bytes.
 * Local section/string targets are checked by content; unmatchable/GC'd cases stay explicit. */
typedef struct ObjInfo {
    const unsigned char *data;
    const Elf64_Shdr *sections;
    const Elf64_Sym *symbols;
    const char *strings;
    size_t section_count, symbol_count;
    size_t *offsets;
    size_t *owners;
} ObjInfo;
typedef struct NameEntry {
    const char *name;
    uint64_t value;
    uint32_t kind;
    unsigned ambiguous;
} NameEntry;
typedef struct RelEntry {
    uint64_t address;
    const Elf64_Rela *relocation;
} RelEntry;

static uint64_t name_hash(const char *s)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static NameEntry *name_slot(NameEntry *table, size_t capacity, const char *name)
{
    size_t pos = (size_t)name_hash(name) & (capacity - 1);
    while (table[pos].name && strcmp(table[pos].name, name))
        pos = (pos + 1) & (capacity - 1);
    return &table[pos];
}

static void add_name(NameEntry *table, size_t capacity, const char *name, uint64_t value, uint32_t kind)
{
    NameEntry *e = name_slot(table, capacity, name);
    if (!e->name) {
        *e = (NameEntry){name, value, kind, 0};
    } else if (e->value != value || e->kind != kind) {
        e->ambiguous = 1;
    }
}

static NameEntry *load_map(const char *path, size_t capacity)
{
    FILE *file = fopen(path, "r");
    NameEntry *table = calloc(capacity, sizeof(*table));
    char line[4096], name[2048], extra;
    unsigned long long vma, lma, size;
    unsigned alignment;
    CHECK(file && table);
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "%llx %llx %llx %u %2047s %c", &vma, &lma, &size, &alignment, name, &extra) != 5)
            continue;
        NameEntry *e = name_slot(table, capacity, name);
        if (!e->name) {
            e->name = strdup(name);
            CHECK(e->name);
            e->value = vma;
        } else if (e->value != vma) {
            e->ambiguous = 1;
        }
    }
    CHECK(!ferror(file));
    fclose(file);
    return table;
}

static int rel_compare(const void *a, const void *b)
{
    const RelEntry *x = a, *y = b;
    return (x->address > y->address) - (x->address < y->address);
}

static int section_category(const Elf64_Shdr *s)
{
    if (!(s->sh_flags & SHF_ALLOC)) return -1;
    if (s->sh_type == SHT_NOBITS) return 3;
    if (s->sh_flags & SHF_EXECINSTR) return 0;
    return (s->sh_flags & SHF_WRITE) ? 2 : 1;
}

static const unsigned char *reference_bytes(const GameLinkObject *ref, uint64_t address, size_t length)
{
    const unsigned char *data = ref->data;
    const Elf64_Ehdr *h = (const Elf64_Ehdr *)data;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + h->e_phoff);
    for (size_t i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || address < ph[i].p_vaddr)
            continue;
        uint64_t offset = address - ph[i].p_vaddr;
        if (offset <= ph[i].p_filesz && length <= ph[i].p_filesz - offset &&
            ph[i].p_offset <= ref->size && offset <= ref->size - ph[i].p_offset &&
            length <= ref->size - ph[i].p_offset - offset)
            return data + ph[i].p_offset + offset;
    }
    return NULL;
}

static void init_object(ObjInfo *o, const GameLinkObject *input)
{
    const Elf64_Ehdr *h = input->data;
    o->data = input->data;
    o->sections = (const Elf64_Shdr *)(o->data + h->e_shoff);
    o->section_count = h->e_shnum;
    o->offsets = malloc(o->section_count * sizeof(*o->offsets));
    o->owners = malloc(o->section_count * sizeof(*o->owners));
    CHECK(o->offsets && o->owners);
    for (size_t j = 0; j < o->section_count; j++) {
        o->offsets[j] = SIZE_MAX;
        o->owners[j] = SIZE_MAX;
        if (o->sections[j].sh_type == SHT_SYMTAB) {
            o->symbols = (const Elf64_Sym *)(o->data + o->sections[j].sh_offset);
            o->symbol_count = o->sections[j].sh_size / sizeof(Elf64_Sym);
            o->strings = (const char *)o->data + o->sections[o->sections[j].sh_link].sh_offset;
        }
    }
    CHECK(o->symbols && o->strings);
    for (size_t j = 1; j < o->symbol_count; j++) {
        const Elf64_Sym *s = &o->symbols[j];
        if (ELF64_ST_BIND(s->st_info) != STB_GLOBAL || ELF64_ST_TYPE(s->st_info) != STT_OBJECT ||
            !s->st_name || !s->st_size || s->st_shndx >= o->section_count)
            continue;
        if (o->owners[s->st_shndx] == SIZE_MAX)
            o->owners[s->st_shndx] = j;
        else
            o->owners[s->st_shndx] = SIZE_MAX - 1;
    }
}

typedef struct CodeSample {
    const char *name;
    size_t offset, size;
    const unsigned char *reference;
    unsigned char *mask;
} CodeSample;

static void compare_code_samples(const GameLinkObject *objects, const ObjInfo *info, size_t count,
                                 const GameLinkObject *reference, NameEntry *ref_names,
                                 const GameLinkImage *image)
{
    CodeSample samples[12] = {{0}};
    size_t n = 0;
    for (size_t i = 0; i < count && n < 12; i++) {
        const char *base = strrchr(objects[i].name, '/');
        base = base ? base + 1 : objects[i].name;
        if (strcmp(base, "main_000.o")) continue;
        const ObjInfo *o = &info[i];
        for (size_t j = 1; j < o->symbol_count && n < 12; j++) {
            const Elf64_Sym *s = &o->symbols[j];
            if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || !s->st_name || !s->st_size ||
                s->st_size > 512 * 1024 || s->st_shndx >= o->section_count ||
                o->offsets[s->st_shndx] == SIZE_MAX)
                continue;
            const char *name = o->strings + s->st_name;
            NameEntry *ref = name_slot(ref_names, 1u << 21, name);
            const unsigned char *expected = ref->name && !ref->ambiguous ?
                reference_bytes(reference, ref->value, s->st_size) : NULL;
            if (!expected) continue;
            samples[n].name = name;
            samples[n].offset = o->offsets[s->st_shndx] + s->st_value;
            samples[n].size = s->st_size;
            samples[n].reference = expected;
            samples[n].mask = calloc(s->st_size, 1);
            CHECK(samples[n].mask);
            n++;
        }
    }
    CHECK(n);
    size_t got_uses = 0, got_relaxed = 0;
    for (size_t i = 0; i < image->fixup_count; i++) {
        const GameLinkFixup *f = &image->fixups[i];
        size_t width = f->type == R_X86_64_64 ? 8 : 4;
        for (size_t j = 0; j < n; j++) {
            CodeSample *s = &samples[j];
            if (f->offset < s->offset || f->offset - s->offset >= s->size) continue;
            size_t at = f->offset - s->offset;
            if (f->type == 41 || f->type == 42) {
                got_uses++;
                if (f->offset >= 2 && image->canonical[f->offset - 2] !=
                    ((const unsigned char *)image->rw)[f->offset - 2])
                    got_relaxed++;
            }
            for (size_t k = 0; k < width && at + k < s->size; k++) s->mask[at + k] = 1;
            if ((f->type == 41 || f->type == 42) && at && f->offset >= 2 &&
                image->canonical[f->offset - 2] == 0xff && image->canonical[f->offset - 1] == 0x25 &&
                ((const unsigned char *)image->rw)[f->offset - 2] == 0xe9)
                s->mask[at - 1] = 1;
        }
    }
    size_t differing = 0, examined = 0;
    for (size_t j = 0; j < n; j++) {
        CodeSample *s = &samples[j];
        const unsigned char *actual = (const unsigned char *)image->rw + s->offset;
        size_t changes = 0, first = 0;
        for (size_t k = 0; k < s->size; k++) {
            if (s->mask[k]) continue;
            examined++;
            if (actual[k] != s->reference[k]) {
                if (!changes) first = k;
                changes++;
            }
        }
        if (changes) {
            differing++;
            printf("code_difference %s bytes=%zu first=+0x%zx ours=%02x pc=%02x\n",
                   s->name, changes, first, actual[first], s->reference[first]);
        }
        free(s->mask);
    }
    printf("code_sample main_000 functions=%zu differing_functions=%zu unmasked_bytes=%zu got=%zu relaxed=%zu\n",
           n, differing, examined, got_uses, got_relaxed);
}


static void compare_relocations(GameLinkObject *objects, size_t count, const GameLinkObject *reference,
                                const GameLinkImage *image, NameEntry *map_names, uintptr_t rw_base,
                                uintptr_t rx_base, const char *label)
{
    const size_t capacity = 1u << 21;
    NameEntry *input_names = calloc(capacity, sizeof(*input_names));
    NameEntry *ref_names = calloc(capacity, sizeof(*ref_names));
    ObjInfo *info = calloc(count, sizeof(*info));
    CHECK(input_names && ref_names && info);
    for (size_t i = 0; i < count; i++) init_object(&info[i], &objects[i]);
    size_t cursor = 0;
    for (int category = 0; category < 4; category++) {
        for (size_t i = 0; i < count; i++) {
            ObjInfo *o = &info[i];
            for (size_t j = 0; j < o->section_count; j++) {
                const Elf64_Shdr *s = &o->sections[j];
                if (section_category(s) != category) continue;
                size_t a = s->sh_addralign ? s->sh_addralign : 1;
                cursor = (cursor + a - 1) & ~(a - 1);
                o->offsets[j] = cursor;
                cursor += s->sh_size;
            }
        }
    }
    CHECK(cursor <= image->got_offset);
    for (size_t i = 0; i < count; i++) {
        ObjInfo *o = &info[i];
        for (size_t j = 1; j < o->symbol_count; j++) {
            const Elf64_Sym *s = &o->symbols[j];
            if ((ELF64_ST_BIND(s->st_info) != STB_GLOBAL && ELF64_ST_BIND(s->st_info) != STB_WEAK) ||
                !s->st_name || s->st_shndx >= o->section_count || o->offsets[s->st_shndx] == SIZE_MAX)
                continue;
            uint32_t kind = (o->sections[s->st_shndx].sh_flags & SHF_WRITE) ? GAME_LINK_TARGET_RW : GAME_LINK_TARGET_RX;
            add_name(input_names, capacity, o->strings + s->st_name, o->offsets[s->st_shndx] + s->st_value, kind);
        }
    }
    const unsigned char *bytes = reference->data;
    const Elf64_Ehdr *rh = (const Elf64_Ehdr *)bytes;
    const Elf64_Shdr *rs = (const Elf64_Shdr *)(bytes + rh->e_shoff);
    const Elf64_Shdr *rnames = &rs[rh->e_shstrndx];
    const char *section_names = (const char *)bytes + rnames->sh_offset;
    const Elf64_Shdr *dynrel = NULL;
    for (size_t i = 0; i < rh->e_shnum; i++) {
        if (!strcmp(section_names + rs[i].sh_name, ".rela.dyn")) dynrel = &rs[i];
        if (rs[i].sh_type != SHT_SYMTAB) continue;
        const Elf64_Sym *symbols = (const Elf64_Sym *)(bytes + rs[i].sh_offset);
        const char *strings = (const char *)bytes + rs[rs[i].sh_link].sh_offset;
        for (size_t j = 1; j < rs[i].sh_size / sizeof(*symbols); j++) {
            const Elf64_Sym *s = &symbols[j];
            if (!s->st_name || s->st_shndx == SHN_UNDEF ||
                (ELF64_ST_TYPE(s->st_info) != STT_OBJECT && ELF64_ST_TYPE(s->st_info) != STT_FUNC))
                continue;
            add_name(ref_names, capacity, strings + s->st_name, s->st_value, 0);
        }
    }
    CHECK(dynrel);
    size_t rel_count = dynrel->sh_size / sizeof(Elf64_Rela);
    const Elf64_Rela *rel = (const Elf64_Rela *)(bytes + dynrel->sh_offset);
    RelEntry *rel_index = malloc(rel_count * sizeof(*rel_index));
    CHECK(rel_index);
    for (size_t i = 0; i < rel_count; i++) rel_index[i] = (RelEntry){rel[i].r_offset, &rel[i]};
    qsort(rel_index, rel_count, sizeof(*rel_index), rel_compare);
    size_t fix = 0, total = 0, writable = 0, readonly = 0, checked = 0, strings_checked = 0;
    size_t missing_source = 0, missing_reference = 0, missing_target = 0, unsupported = 0, mismatch = 0;
    size_t map_missing = 0;
    for (size_t i = 0; i < count; i++) {
        ObjInfo *o = &info[i];
        for (size_t j = 0; j < o->section_count; j++) {
            const Elf64_Shdr *r = &o->sections[j];
            if (r->sh_type != SHT_RELA || o->offsets[r->sh_info] == SIZE_MAX) continue;
            const Elf64_Rela *entries = (const Elf64_Rela *)(o->data + r->sh_offset);
            const Elf64_Shdr *section = &o->sections[r->sh_info];
            for (size_t k = 0; k < r->sh_size / sizeof(*entries); k++, fix++) {
                const Elf64_Rela *entry = &entries[k];
                CHECK(fix < image->fixup_count);
                const GameLinkFixup *f = &image->fixups[fix];
                CHECK(f->offset == o->offsets[r->sh_info] + entry->r_offset && f->type == ELF64_R_TYPE(entry->r_info));
                if (f->type != R_X86_64_64 || (section->sh_flags & SHF_EXECINSTR)) continue;
                total++;
                if (section->sh_flags & SHF_WRITE)
                    writable++;
                else
                    readonly++;
                size_t owner = o->owners[r->sh_info];
                if (owner >= o->symbol_count) { missing_source++; continue; }
                const Elf64_Sym *src = &o->symbols[owner];
                if (entry->r_offset < src->st_value || entry->r_offset - src->st_value >= src->st_size) {
                    missing_source++; continue;
                }
                const char *source_name = o->strings + src->st_name;
                NameEntry *reference_source = name_slot(ref_names, capacity, source_name);
                if (!reference_source->name || reference_source->ambiguous) { missing_reference++; continue; }
                NameEntry *map_source = name_slot(map_names, capacity, source_name);
                if (!map_source->name || map_source->ambiguous) {
                    map_missing++;
                } else if (map_source->value != reference_source->value) {
                    if (mismatch++ < 20) fprintf(stderr, "%s: map source differs %s\n", label, source_name);
                }
                RelEntry key = {reference_source->value + entry->r_offset - src->st_value, NULL};
                RelEntry *found = bsearch(&key, rel_index, rel_count, sizeof(*rel_index), rel_compare);
                if (!found || ELF64_R_TYPE(found->relocation->r_info) != R_X86_64_RELATIVE) {
                    missing_reference++; continue;
                }
                const Elf64_Sym *dst = &o->symbols[ELF64_R_SYM(entry->r_info)];
                uint64_t actual;
                memcpy(&actual, (const unsigned char *)image->rw + f->offset, sizeof actual);
                if (ELF64_ST_TYPE(dst->st_info) == STT_SECTION && dst->st_shndx < o->section_count &&
                    (o->sections[dst->st_shndx].sh_flags & SHF_STRINGS) && entry->r_addend >= 0 &&
                    (uint64_t)entry->r_addend < o->sections[dst->st_shndx].sh_size) {
                    const char *expected = (const char *)o->data + o->sections[dst->st_shndx].sh_offset +
                                           entry->r_addend;
                    size_t max = o->sections[dst->st_shndx].sh_size - (size_t)entry->r_addend;
                    const char *end = memchr(expected, 0, max);
                    const unsigned char *ref_text = end ? reference_bytes(reference, found->relocation->r_addend,
                                                                           (size_t)(end - expected) + 1) : NULL;
                    uint64_t expected_pointer = rx_base + o->offsets[dst->st_shndx] + entry->r_addend;
                    if (!ref_text) { unsupported++; continue; }
                    checked++;
                    strings_checked++;
                    if (actual != expected_pointer || memcmp(ref_text, expected, (size_t)(end - expected) + 1)) {
                        if (mismatch++ < 20) fprintf(stderr, "%s: string mismatch %s+0x%llx\n", label, source_name,
                                                     (unsigned long long)(entry->r_offset - src->st_value));
                    }
                    continue;
                }
                if (!dst->st_name) { unsupported++; continue; }
                const char *target_name = o->strings + dst->st_name;
                NameEntry *target = name_slot(input_names, capacity, target_name);
                NameEntry *ref_target = name_slot(ref_names, capacity, target_name);
                if (!target->name || target->ambiguous || !ref_target->name || ref_target->ambiguous) {
                    missing_target++; continue;
                }
                NameEntry *map_target = name_slot(map_names, capacity, target_name);
                if (!map_target->name || map_target->ambiguous) {
                    map_missing++;
                } else if (map_target->value != ref_target->value) {
                    if (mismatch++ < 20) fprintf(stderr, "%s: map target differs %s\n", label, target_name);
                }
                uint64_t expected_pointer = (target->kind == GAME_LINK_TARGET_RW ? rw_base : rx_base) +
                                            target->value + entry->r_addend;
                uint64_t expected_reference = ref_target->value + entry->r_addend;
                checked++;
                if (actual != expected_pointer || (uint64_t)found->relocation->r_addend != expected_reference) {
                    if (mismatch++ < 20)
                        fprintf(stderr, "%s: %s+0x%llx -> %s expected=%llx actual=%llx ref=%llx refsym=%llx\n",
                                label, source_name, (unsigned long long)(entry->r_offset - src->st_value), target_name,
                                (unsigned long long)expected_pointer, (unsigned long long)actual,
                                (unsigned long long)found->relocation->r_addend,
                                (unsigned long long)expected_reference);
                }
            }
        }
    }
    CHECK(fix == image->fixup_count);
    printf("%s data64_total=%zu writable=%zu readonly=%zu checked=%zu strings=%zu mismatch=%zu no_source=%zu "
           "no_reference=%zu no_target=%zu unsupported=%zu map_missing=%zu\n", label, total, writable, readonly,
           checked, strings_checked, mismatch, missing_source, missing_reference, missing_target, unsupported,
           map_missing);
    if (!strcmp(label, "fresh"))
        compare_code_samples(objects, info, count, reference, ref_names, image);
    for (size_t i = 0; i < count; i++) { free(info[i].offsets); free(info[i].owners); }
    free(info); free(input_names); free(ref_names); free(rel_index);
    CHECK(total && checked == total && !mismatch && !map_missing);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 5);
    GameLinkObject reference = load(argv[1]);
    NameEntry *map_names = load_map(argv[2], 1u << 21);
    size_t count = (size_t)argc - 4;
    GameLinkObject *objects = calloc(count, sizeof(*objects));
    CHECK(objects);
    for (size_t i = 0; i < count; i++) objects[i] = load(argv[i + 4]);
    size_t capacity = (size_t)512 * 1024 * 1024;
    uintptr_t rw_a = UINT64_C(0x402100000), rx_a = UINT64_C(0x154c0000);
    uintptr_t rw_b = UINT64_C(0x502100000), rx_b = UINT64_C(0x254c0000);
    void *a = mmap((void *)rw_a, capacity, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *b = mmap((void *)rw_b, capacity, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED && (uintptr_t)a == rw_a && (uintptr_t)b == rw_b);
    size_t export_count;
    GameExport *exports = manifest_exports(argv[3], (void *)(rx_a + 0x100), &export_count);
    Buffer buffer = {a, capacity, rx_a};
    GameLinkAllocator allocator = {&buffer, allocate_image, release_image};
    GameLinkImage image;
    char error[256];
    if (!gamecode_link(objects, count, exports, export_count, &allocator, &image, error, sizeof error)) {
        fprintf(stderr, "console-like link failed: %s\n", error);
        return 1;
    }
    printf("view A rw=%llx rx=%llx size=%zu\n", (unsigned long long)rw_a, (unsigned long long)rx_a, image.size);
    GotStats got = got_stats(&image);
    printf("console_got uses=%zu relaxed=%zu mov=%zu call=%zu jump=%zu active_slots=%zu reserved_slots=%zu\n",
           got.uses, got.mov + got.call + got.jump, got.mov, got.call, got.jump, got.slots_used, image.got_count);
    printf("console_got targets_rx=%zu targets_rw=%zu targets_export=%zu targets_absolute=%zu "
           "(exports are near-address dummies)\n",
           got.target_rx, got.target_rw, got.target_export, got.target_absolute);
    compare_relocations(objects, count, &reference, &image, map_names, rw_a, rx_a, "fresh");
    if (!gamecode_link_apply_fixups(&image, b, rx_b, exports, export_count, error, sizeof error)) {
        fprintf(stderr, "console-like replay failed: %s\n", error);
        return 1;
    }
    image.rw = b;
    printf("view B rw=%llx rx=%llx\n", (unsigned long long)rw_b, (unsigned long long)rx_b);
    compare_relocations(objects, count, &reference, &image, map_names, rw_b, rx_b, "replay");
    gamecode_link_free_metadata(&image);
    for (size_t i = 0; i < export_count; i++) free((void *)exports[i].name);
    free(exports);
    for (size_t i = 0; i < count; i++) munmap((void *)objects[i].data, objects[i].size);
    free(objects);
    for (size_t i = 0; i < (1u << 21); i++) free((void *)map_names[i].name);
    free(map_names);
    munmap((void *)reference.data, reference.size);
    munmap(a, capacity);
    munmap(b, capacity);
    return 0;
}
