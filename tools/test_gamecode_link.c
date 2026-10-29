#define _GNU_SOURCE
#include "../runtime/gamecode_link.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct Buffer { void *memory; size_t capacity; uintptr_t rx; } Buffer;

static int allocate_image(void *context, size_t size, size_t alignment, void **rw, uintptr_t *rx)
{
    Buffer *b = context;
    if (size > b->capacity || ((uintptr_t)b->memory & (alignment - 1)))
        return 0;
    *rw = b->memory;
    *rx = b->rx;
    return 1;
}

static void release_image(void *context, void *rw, size_t size)
{
    (void)context;
    (void)rw;
    (void)size;
}

static GameLinkObject load(const char *name)
{
    int fd = open(name, O_RDONLY);
    struct stat st;
    CHECK(fd >= 0 && !fstat(fd, &st) && st.st_size > 0);
    void *data = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(data != MAP_FAILED);
    close(fd);
    return (GameLinkObject){data, (size_t)st.st_size, name};
}

static int compare_exports(const void *a, const void *b)
{
    return strcmp(((const GameExport *)a)->name, ((const GameExport *)b)->name);
}

static GameExport *object_symbols(GameLinkObject *objects, size_t count, int undefined, size_t *result_count)
{
    GameExport *result = NULL;
    size_t used = 0, cap = 0;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *data = objects[i].data;
        const Elf64_Ehdr *h = (const Elf64_Ehdr *)data;
        const Elf64_Shdr *sections = (const Elf64_Shdr *)(data + h->e_shoff);
        for (size_t j = 0; j < h->e_shnum; j++) {
            if (sections[j].sh_type != SHT_SYMTAB)
                continue;
            const Elf64_Sym *syms = (const Elf64_Sym *)(data + sections[j].sh_offset);
            const char *strings = (const char *)data + sections[sections[j].sh_link].sh_offset;
            for (size_t k = 1; k < sections[j].sh_size / sizeof(*syms); k++) {
                unsigned bind = ELF64_ST_BIND(syms[k].st_info);
                if ((bind != STB_GLOBAL && bind != STB_WEAK) || !syms[k].st_name ||
                    (syms[k].st_shndx == SHN_UNDEF) != undefined)
                    continue;
                if (used == cap) {
                    cap = cap ? cap * 2 : 128;
                    result = realloc(result, cap * sizeof(*result));
                    CHECK(result);
                }
                result[used++] = (GameExport){strings + syms[k].st_name, NULL};
            }
        }
    }
    qsort(result, used, sizeof(*result), compare_exports);
    size_t out = 0;
    for (size_t i = 0; i < used; i++) {
        if (!out || strcmp(result[i].name, result[out - 1].name))
            result[out++] = result[i];
    }
    *result_count = out;
    return result;
}

static GameExport *manifest_exports(const char *path, void *dummy, size_t *count)
{
    FILE *file = fopen(path, "r");
    GameExport *exports = NULL;
    char line[512], name[256];
    size_t used = 0;
    CHECK(file);
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, " {\"%255[^\"]\"", name) != 1)
            continue;
        GameExport *next = realloc(exports, (used + 1) * sizeof(*exports));
        CHECK(next);
        exports = next;
        exports[used].name = strdup(name);
        CHECK(exports[used].name);
        exports[used++].address = dummy;
    }
    CHECK(!ferror(file));
    fclose(file);
    qsort(exports, used, sizeof(*exports), compare_exports);
    *count = used;
    return exports;
}

static int external(int x) { return x * 2; }
static int data_value = 5;

static void expect_failure(GameLinkObject *objects, size_t count, GameExport *exports, size_t export_count,
                           GameLinkAllocator *a, const char *expected)
{
    GameLinkImage image;
    char error[256];
    CHECK(!gamecode_link(objects, count, exports, export_count, a, &image, error, sizeof error));
    if (!strstr(error, expected)) {
        fprintf(stderr, "expected %s, got %s\n", expected, error);
        exit(1);
    }
}

static void synthetic(const char *path, GameLinkAllocator *a)
{
    GameLinkObject object = load(path);
    GameExport exports[] = {{"test_data", &data_value}, {"test_external", (void *)external}};
    GameLinkImage image;
    char error[256];
    if (!gamecode_link(&object, 1, exports, 2, a, &image, error, sizeof error)) {
        fprintf(stderr, "link failed: %s\n", error);
        exit(1);
    }
    CHECK(((int (*)(void))image.game_version)() == 34);
    CHECK(((uintptr_t)image.game_modules & 63) == 0);
    CHECK(*(int *)image.game_site_count == 0);
    CHECK(*(void **)image.game_sites == NULL);
    __int128 distance = (__int128)(uintptr_t)external - (__int128)image.rx;
    if (distance < INT32_MIN || distance > INT32_MAX)
        CHECK(image.thunk_count > 0);
    CHECK(image.got_count > 0 && image.text_size > 0 && image.bss_size >= 128);
    GameLinkObject pair[] = {object, object};
    expect_failure(pair, 2, exports, 2, a, "duplicate strong");
    expect_failure(&object, 1, exports, 1, a, "unresolved symbol");
    unsigned char *bytes = (unsigned char *)object.data;
    Elf64_Ehdr *h = (Elf64_Ehdr *)bytes;
    Elf64_Shdr *sections = (Elf64_Shdr *)(bytes + h->e_shoff);
    Elf64_Shdr *rela = NULL, *symtab = NULL, *alloc = NULL;
    for (size_t i = 0; i < h->e_shnum; i++) {
        if (sections[i].sh_type == SHT_RELA && (sections[sections[i].sh_info].sh_flags & SHF_ALLOC) &&
            sections[i].sh_size)
            rela = &sections[i];
        if (sections[i].sh_type == SHT_SYMTAB)
            symtab = &sections[i];
        if ((sections[i].sh_flags & SHF_ALLOC) && sections[i].sh_type == SHT_PROGBITS)
            alloc = &sections[i];
    }
    CHECK(rela && symtab && alloc);
    Elf64_Rela *r = (Elf64_Rela *)(bytes + rela->sh_offset);
    Elf64_Xword saved_info = r->r_info;
    r->r_info = ELF64_R_INFO(ELF64_R_SYM(saved_info), R_X86_64_32);
    expect_failure(&object, 1, exports, 2, a, "unsupported relocation");
    r->r_info = saved_info;
    const char *strings = (const char *)bytes + sections[symtab->sh_link].sh_offset;
    Elf64_Sym *all_symbols = (Elf64_Sym *)(bytes + symtab->sh_offset);
    Elf64_Rela *data_relocation = NULL;
    for (size_t i = 0; i < h->e_shnum; i++) {
        if (sections[i].sh_type != SHT_RELA)
            continue;
        Elf64_Rela *entries = (Elf64_Rela *)(bytes + sections[i].sh_offset);
        for (size_t j = 0; j < sections[i].sh_size / sizeof(*entries); j++) {
            size_t index = ELF64_R_SYM(entries[j].r_info);
            if (!strcmp(strings + all_symbols[index].st_name, "test_data"))
                data_relocation = &entries[j];
        }
    }
    CHECK(data_relocation);
    Elf64_Xword data_info = data_relocation->r_info;
    data_relocation->r_info = ELF64_R_INFO(ELF64_R_SYM(data_info), R_X86_64_PC32);
    void *saved_address = exports[0].address;
    exports[0].address = (void *)1;
    expect_failure(&object, 1, exports, 2, a, "out of range");
    exports[0].address = saved_address;
    data_relocation->r_info = data_info;
    Elf64_Sym *symbols = (Elf64_Sym *)(bytes + symtab->sh_offset);
    Elf64_Half saved_index = symbols[1].st_shndx;
    symbols[1].st_shndx = SHN_COMMON;
    expect_failure(&object, 1, exports, 2, a, "common symbol");
    symbols[1].st_shndx = saved_index;
    Elf64_Xword saved_flags = alloc->sh_flags;
    alloc->sh_flags |= SHF_TLS;
    expect_failure(&object, 1, exports, 2, a, "TLS section");
    alloc->sh_flags = saved_flags | SHF_GROUP;
    expect_failure(&object, 1, exports, 2, a, "COMDAT");
    alloc->sh_flags = saved_flags;
    Elf64_Word saved_type = alloc->sh_type;
    alloc->sh_type = SHT_INIT_ARRAY;
    expect_failure(&object, 1, exports, 2, a, ".init_array");
    alloc->sh_type = saved_type;
    GameLinkObject weak_object = load(path);
    unsigned char *weak_bytes = (unsigned char *)weak_object.data;
    Elf64_Ehdr *weak_header = (Elf64_Ehdr *)weak_bytes;
    Elf64_Shdr *weak_sections = (Elf64_Shdr *)(weak_bytes + weak_header->e_shoff);
    for (size_t i = 0; i < weak_header->e_shnum; i++) {
        if (weak_sections[i].sh_type != SHT_SYMTAB)
            continue;
        Elf64_Sym *weak_symbols = (Elf64_Sym *)(weak_bytes + weak_sections[i].sh_offset);
        const char *weak_strings = (const char *)weak_bytes + weak_sections[weak_sections[i].sh_link].sh_offset;
        for (size_t j = 1; j < weak_sections[i].sh_size / sizeof(*weak_symbols); j++) {
            Elf64_Sym *sym = &weak_symbols[j];
            if (ELF64_ST_BIND(sym->st_info) != STB_GLOBAL || sym->st_shndx == SHN_UNDEF)
                continue;
            sym->st_info = ELF64_ST_INFO(STB_WEAK, ELF64_ST_TYPE(sym->st_info));
            if (!strcmp(weak_strings + sym->st_name, "game_module_count")) {
                int value = 17;
                memcpy(weak_bytes + weak_sections[sym->st_shndx].sh_offset + sym->st_value, &value, sizeof value);
            }
        }
    }
    GameLinkObject mixed[] = {weak_object, object};
    CHECK(gamecode_link(mixed, 2, exports, 2, a, &image, error, sizeof error));
    CHECK(*(int *)image.game_module_count == 7);
    CHECK(((int (*)(void))image.game_version)() == 34);
    munmap((void *)weak_object.data, weak_object.size);
    munmap((void *)object.data, object.size);
    puts("synthetic linker tests passed");
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    size_t capacity = (size_t)1024 * 1024 * 1024;
    int fd = memfd_create("gamecode-link-test", 0);
    CHECK(fd >= 0 && !ftruncate(fd, capacity));
    void *memory = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *executable = mmap(NULL, capacity, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    CHECK(memory != MAP_FAILED && executable != MAP_FAILED && memory != executable);
    Buffer buffer = {memory, capacity, (uintptr_t)executable};
    GameLinkAllocator allocator = {&buffer, allocate_image, release_image};
    if (!strcmp(argv[1], "--synthetic")) {
        synthetic(argv[2], &allocator);
    } else {
        CHECK(!strcmp(argv[1], "--real"));
        CHECK(argc >= 4);
        size_t count = (size_t)argc - 3;
        GameLinkObject *objects = calloc(count, sizeof(*objects));
        CHECK(objects);
        for (size_t i = 0; i < count; i++)
            objects[i] = load(argv[i + 3]);
        size_t export_count, undefined_count, defined_count, missing = 0;
        GameExport *exports = manifest_exports(argv[2], memory, &export_count);
        GameExport *undefined = object_symbols(objects, count, 1, &undefined_count);
        GameExport *defined = object_symbols(objects, count, 0, &defined_count);
        CHECK(export_count);
        for (size_t i = 0; i < undefined_count; i++) {
            if (!bsearch(&undefined[i], defined, defined_count, sizeof(*defined), compare_exports) &&
                !bsearch(&undefined[i], exports, export_count, sizeof(*exports), compare_exports)) {
                fprintf(stderr, "undefined outside exports: %s\n", undefined[i].name);
                missing++;
            }
        }
        printf("exports=%zu external_missing=%zu\n", export_count, missing);
        free(undefined);
        free(defined);
        struct timespec begin, end;
        struct rusage usage;
        GameLinkImage image;
        char error[256];
        clock_gettime(CLOCK_MONOTONIC, &begin);
        if (!gamecode_link(objects, count, exports, export_count, &allocator, &image, error, sizeof error)) {
            fprintf(stderr, "real link failed: %s\n", error);
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        getrusage(RUSAGE_SELF, &usage);
        printf("objects=%zu text=%zu rodata=%zu data=%zu bss=%zu image=%zu got=%zu thunks=%zu "
               "seconds=%.3f peak_rss_kib=%ld\n", count, image.text_size, image.rodata_size,
               image.data_size, image.bss_size, image.size, image.got_count, image.thunk_count,
               (end.tv_sec - begin.tv_sec) + (end.tv_nsec - begin.tv_nsec) / 1e9, usage.ru_maxrss);
        for (size_t i = 0; i < export_count; i++)
            free((void *)exports[i].name);
        free(exports);
        for (size_t i = 0; i < count; i++)
            munmap((void *)objects[i].data, objects[i].size);
        free(objects);
    }
    munmap(executable, capacity);
    munmap(memory, capacity);
    return 0;
}
