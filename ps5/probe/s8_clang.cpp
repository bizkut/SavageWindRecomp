// In-process Clang scaling probe for the M12 compiler spike.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <elf.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef __PROSPERO__
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
extern "C" int sceSystemServiceLoadExec(const char *, const char *const *);
extern "C" int sceKernelDebugOutText(int, const char *);
#endif

// A title's stdout doesn't reach the console log; use the kernel log as the game does.
static void say(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef __PROSPERO__
    sceKernelDebugOutText(0, line);
#else
    std::fputs(line, stdout);
    std::fflush(stdout);
#endif
}

static int finish(int code) {
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sleep(1);
#endif
    return code;
}

static uint64_t now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

struct Result {
    bool ok = false;
    uint64_t ns = 0;
    size_t bytes = 0;
    std::array<uint8_t, 32> sha{};
    std::string diagnostics;
    llvm::SmallVector<char, 0> object;
};

static Result compile(const std::vector<const char *> &args, bool capture = false) {
    Result result;
    uint64_t start = now_ns();
    llvm::SmallVector<char, 0> object;
    llvm::raw_string_ostream diag_stream(result.diagnostics);
    auto diag_options = llvm::makeIntrusiveRefCnt<clang::DiagnosticOptions>();
    clang::CompilerInstance compiler;
    compiler.createDiagnostics(new clang::TextDiagnosticPrinter(diag_stream, diag_options.get()));
    if (!compiler.hasDiagnostics()) {
        result.diagnostics = "no diagnostics engine";
        return result;
    }
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*invocation, args, compiler.getDiagnostics())) {
        diag_stream.flush();
        return result;
    }
    // BackendUtil's global command-line parser is not thread-safe; these options
    // are the only ones that invoke it, and neither is used by this probe.
    if (!invocation->getCodeGenOpts().DebugPass.empty() ||
        !invocation->getCodeGenOpts().LimitFloatPrecision.empty()) {
        result.diagnostics = "unsupported global LLVM backend options";
        return result;
    }
    compiler.setInvocation(invocation);
    compiler.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(object));
    clang::EmitObjAction action;
    result.ok = compiler.ExecuteAction(action);
    result.ns = now_ns() - start;
    diag_stream.flush();
    result.bytes = object.size();
    if (result.ok && !object.empty()) {
        result.sha = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t *>(object.data()), object.size()));
        if (capture) result.object.swap(object);
    } else {
        result.ok = false;
    }
    return result;
}

static std::string hex(const std::array<uint8_t, 32> &sha) {
    static const char digits[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < sha.size(); ++i) {
        s[2 * i] = digits[sha[i] >> 4];
        s[2 * i + 1] = digits[sha[i] & 15];
    }
    return s;
}

// Section extents map differing bytes, including blocks crossing boundaries.
// Bytes outside them are ELF headers, section headers, or padding.
static void log_sections(const llvm::SmallVector<char, 0> &object, size_t offset, size_t length) {
    Elf64_Ehdr eh{};
    if (object.size() < sizeof eh) return;
    std::memcpy(&eh, object.data(), sizeof eh);
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_shentsize != sizeof(Elf64_Shdr) ||
        eh.e_shstrndx >= eh.e_shnum || eh.e_shoff > object.size() ||
        eh.e_shnum > (object.size() - eh.e_shoff) / sizeof(Elf64_Shdr)) {
        say("[s8] sections block=%zx invalid_elf=1\n", offset);
        return;
    }
    auto section = [&](size_t i) {
        Elf64_Shdr sh{};
        std::memcpy(&sh, object.data() + eh.e_shoff + i * sizeof sh, sizeof sh);
        return sh;
    };
    Elf64_Shdr names = section(eh.e_shstrndx);
    if (names.sh_offset > object.size() || names.sh_size > object.size() - names.sh_offset) {
        say("[s8] sections block=%zx invalid_names=1\n", offset);
        return;
    }
    for (size_t i = 0; i < eh.e_shnum; ++i) {
        Elf64_Shdr sh = section(i);
        if (sh.sh_type == SHT_NOBITS || !sh.sh_size || sh.sh_offset > object.size() ||
            sh.sh_size > object.size() - sh.sh_offset ||
            sh.sh_offset >= offset + length || sh.sh_offset + sh.sh_size <= offset) continue;
        const char *name = "<invalid>";
        if (sh.sh_name < names.sh_size) {
            const char *candidate = object.data() + names.sh_offset + sh.sh_name;
            if (std::memchr(candidate, 0, names.sh_size - sh.sh_name)) name = candidate;
        }
        say("[s8] section block=%zx range=%llx-%llx name=%s\n", offset,
            (unsigned long long)sh.sh_offset, (unsigned long long)(sh.sh_offset + sh.sh_size), name);
    }
}

static bool report_blocks(const Result &result, const std::string &root) {
    constexpr size_t block_size = 4096;
    const auto &object = result.object;
    if (!result.ok || object.size() != result.bytes) {
        say("[s8] blocks unavailable: no captured object\n");
        return false;
    }
    const std::string blocks_path = root + "/host-blocks.txt";
#ifdef __PROSPERO__
    FILE *blocks = std::fopen(blocks_path.c_str(), "r");
#else
    FILE *blocks = std::fopen(blocks_path.c_str(), "w");
    const std::string object_path = root + "/host-main_040.o";
    FILE *out = std::fopen(object_path.c_str(), "wb");
    bool saved = out && std::fwrite(object.data(), 1, object.size(), out) == object.size();
    if (out && std::fclose(out)) saved = false;
    if (!saved) say("[s8] failed: cannot write %s\n", object_path.c_str());
#endif
    if (!blocks) {
        say("[s8] failed: cannot open %s\n", blocks_path.c_str());
        return false;
    }
    bool valid = true;
    size_t differing = 0;
    for (size_t offset = 0; offset < object.size(); offset += block_size) {
        size_t length = std::min(block_size, object.size() - offset);
        auto digest = llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(
            reinterpret_cast<const uint8_t *>(object.data() + offset), length));
        std::string prefix = hex(digest).substr(0, 16);
#ifdef __PROSPERO__
        unsigned long long ref_offset = 0;
        size_t ref_length = 0;
        char reference[17]{};
        if (std::fscanf(blocks, "%llx %zu %16s", &ref_offset, &ref_length, reference) != 3 ||
            ref_offset != offset || ref_length != length || std::strlen(reference) != 16) {
            say("[s8] failed: invalid host block table at offset=%zx\n", offset);
            valid = false;
            break;
        }
        bool different = prefix != reference;
        say("[s8] block offset=%zx length=%zu sha256=%s host=%s match=%d\n",
            offset, length, prefix.c_str(), reference, !different);
        if (!different) continue;
        ++differing;
        log_sections(object, offset, length);
        static const char digits[] = "0123456789abcdef";
        for (size_t line_offset = offset; line_offset < offset + length; line_offset += 64) {
            char bytes[64 * 3 + 1];
            size_t count = std::min(size_t(64), offset + length - line_offset);
            for (size_t i = 0; i < count; ++i) {
                unsigned char b = static_cast<unsigned char>(object[line_offset + i]);
                bytes[3 * i] = digits[b >> 4];
                bytes[3 * i + 1] = digits[b & 15];
                bytes[3 * i + 2] = i + 1 == count ? '\0' : ' ';
            }
            say("[s8] hex %zx %s\n", line_offset, bytes);
        }
#else
        say("[s8] block offset=%zx length=%zu sha256=%s\n", offset, length, prefix.c_str());
        if (std::fprintf(blocks, "%zx %zu %s\n", offset, length, prefix.c_str()) < 0) valid = false;
#endif
    }
#ifdef __PROSPERO__
    char extra[2];
    if (valid && std::fscanf(blocks, "%1s", extra) == 1) valid = false;
#else
    if (!saved) valid = false;
    say("[s8] host files blocks=%s object=%s\n", blocks_path.c_str(), object_path.c_str());
#endif
    if (std::fclose(blocks)) valid = false;
    say("[s8] blocks done valid=%d differing=%zu\n", valid, differing);
    return valid;
}
struct Memory {
#ifdef __PROSPERO__
    size_t flex = 0, flex_total = 0, direct = 0;
    int flex_rc = 0, config_rc = 0, direct_rc = 0;
    int64_t direct_total = 0;
    struct ps5_heap_stats heap{};
#endif
};

static Memory memory_sample() {
    Memory m;
#ifdef __PROSPERO__
    ps5_heap_stats(&m.heap);
    m.flex_rc = sceKernelAvailableFlexibleMemorySize(&m.flex);
    m.config_rc = sceKernelConfiguredFlexibleMemorySize(&m.flex_total);
    int64_t start = 0;
    m.direct_total = sceKernelGetDirectMemorySize();
    m.direct_rc = sceKernelAvailableDirectMemorySize(0, m.direct_total, PS5_KERNEL_DIRECT_ALIGNMENT,
                                                      &start, &m.direct);
#endif
    return m;
}

static void memory_log(int n, const char *stage, const Memory &m) {
#ifdef __PROSPERO__
    say("[s8] memory n=%d stage=%s flex_free=%llu flex_total=%llu flex_rc=%d config_rc=%d "
        "direct_largest=%llu direct_total=%lld direct_rc=%d heap_mapped=%llu heap_peak_lifetime=%llu "
        "heap_segments=%u heap_arenas=%u heap_libc_fallbacks=%llu\n", n, stage,
        (unsigned long long)m.flex, (unsigned long long)m.flex_total, m.flex_rc, m.config_rc,
        (unsigned long long)m.direct, (long long)m.direct_total, m.direct_rc,
        (unsigned long long)m.heap.mapped_bytes, (unsigned long long)m.heap.peak_bytes,
        m.heap.segments, m.heap.arenas, m.heap.libc_fallbacks);
#else
    (void)m;
    say("[s8] memory n=%d stage=%s ps5_memory=unavailable\n", n, stage);
#endif
}

struct Monitor {
    std::atomic<bool> stop{false};
    Memory peak;
#ifdef __PROSPERO__
    bool have_flex = false, have_direct = false;
#endif
    unsigned samples = 0;
};

static void update_peak(Monitor &monitor, const Memory &m) {
#ifdef __PROSPERO__
    // The minima and maxima are independent and may occur in different samples.
    Memory &p = monitor.peak;
    if (monitor.samples == 0) p = m;
    else {
        if (m.heap.mapped_bytes > p.heap.mapped_bytes) p.heap.mapped_bytes = m.heap.mapped_bytes;
        if (m.heap.peak_bytes > p.heap.peak_bytes) p.heap.peak_bytes = m.heap.peak_bytes;
        if (m.heap.segments > p.heap.segments) p.heap.segments = m.heap.segments;
        if (m.heap.arenas > p.heap.arenas) p.heap.arenas = m.heap.arenas;
        if (m.heap.libc_fallbacks > p.heap.libc_fallbacks) p.heap.libc_fallbacks = m.heap.libc_fallbacks;
        if (m.flex_rc) p.flex_rc = m.flex_rc;
        if (m.config_rc) p.config_rc = m.config_rc;
        if (m.direct_rc) p.direct_rc = m.direct_rc;
    }
    if (!m.flex_rc && (!monitor.have_flex || m.flex < p.flex)) p.flex = m.flex;
    if (!m.direct_rc && (!monitor.have_direct || m.direct < p.direct)) p.direct = m.direct;
    monitor.have_flex |= !m.flex_rc;
    monitor.have_direct |= !m.direct_rc;
#else
    (void)m;
#endif
    ++monitor.samples;
}

static void *monitor_main(void *opaque) {
    Monitor &m = *static_cast<Monitor *>(opaque);
    while (!m.stop.load(std::memory_order_acquire)) {
        update_peak(m, memory_sample());
        timespec delay{0, 100000000};
        nanosleep(&delay, nullptr);
    }
    update_peak(m, memory_sample());
    return nullptr;
}

struct Worker {
    const std::vector<const char *> *args;
    std::atomic<bool> *go;
    bool capture;
    Result result;
};

static void *worker_main(void *opaque) {
    Worker &w = *static_cast<Worker *>(opaque);
    while (!w.go->load(std::memory_order_acquire)) {
        timespec delay{0, 1000000};
        nanosleep(&delay, nullptr);
    }
    w.result = compile(*w.args, w.capture);
    return nullptr;
}

static bool run(int n, const std::vector<const char *> &args, const Result *reference, Result *single) {
    Memory before = memory_sample();
    Monitor monitor;
    pthread_t monitor_thread;
    int rc = pthread_create(&monitor_thread, nullptr, monitor_main, &monitor);
    if (rc) { say("[s8] failed: monitor pthread_create rc=%d\n", rc); return false; }
    std::atomic<bool> go{false};
    std::vector<Worker> workers(n);
    std::vector<pthread_t> threads(n);
    pthread_attr_t attr;
    rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
        if (rc) pthread_attr_destroy(&attr);
    }
    int created = 0;
    if (!rc) {
        for (; created < n; ++created) {
            workers[created].args = &args;
            workers[created].go = &go;
            workers[created].capture = single != nullptr;
            rc = pthread_create(&threads[created], &attr, worker_main, &workers[created]);
            if (rc) break;
        }
        pthread_attr_destroy(&attr);
    }
    uint64_t start = now_ns();
    go.store(true, std::memory_order_release);
    for (int i = 0; i < created; ++i) pthread_join(threads[i], nullptr);
    uint64_t wall = now_ns() - start;
    monitor.stop.store(true, std::memory_order_release);
    pthread_join(monitor_thread, nullptr);
    Memory after = memory_sample();
    if (rc) say("[s8] failed: worker setup n=%d created=%d rc=%d\n", n, created, rc);
    uint64_t sum = 0;
    int ok = 0, identical = 0;
    for (int i = 0; i < created; ++i) {
        const Result &r = workers[i].result;
        sum += r.ns;
        if (r.ok) ++ok;
        if (r.ok && (!reference || (r.bytes == reference->bytes && r.sha == reference->sha))) ++identical;
        if (!r.diagnostics.empty()) say("[s8] diagnostics n=%d worker=%d: %.1500s\n", n, i, r.diagnostics.c_str());
    }
    say("[s8] scaling n=%d wall_ms=%.3f sum_compile_ms=%.3f objects_ok=%d/%d identical=%d/%d samples=%u\n",
        n, wall / 1e6, sum / 1e6, ok, n, identical, n, monitor.samples);
    memory_log(n, "before", before);
    memory_log(n, "peak_extrema_100ms", monitor.peak);
    memory_log(n, "after", after);
    if (single && created == 1) *single = std::move(workers[0].result);
    return !rc && ok == n && identical == n;
}

int main(int argc, char **argv) {
#ifdef __PROSPERO__
    (void)argc; (void)argv;
    const char *base = "/data/savagewind/s8";
#else
    const char *base = argc > 1 ? argv[1] : "build/ps5/s8-input";
#endif
    const std::string root(base), input = root + "/main_040.c";
    struct stat st{};
    say("[s8] start: %s llvm=%s threads_enabled=%d\n", input.c_str(), LLVM_VERSION_STRING, LLVM_ENABLE_THREADS);
    if (!LLVM_ENABLE_THREADS || stat(input.c_str(), &st) != 0) {
        say("[s8] failed: LLVM threads disabled or cannot stat input\n");
        return finish(1);
    }
    // Target registration mutates global registries and must precede all workers.
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();
    std::vector<std::string> args = {
        "-triple", "x86_64-sie-ps5", "-emit-obj", "-mrelocation-model", "pic", "-pic-level", "2",
        "-fhalf-no-semantic-interposition", "-mframe-pointer=non-leaf", "-relaxed-aliasing",
        "-fdenormal-fp-math=preserve-sign,preserve-sign", "-ffp-contract=on", "-fno-rounding-math",
        "-mconstructor-aliases", "-fno-plt", "-funwind-tables=2", "-fno-use-init-array",
        "-fvisibility-global-new-delete=source", "-fvisibility-from-dllstorageclass",
        "-fvisibility-dllexport=protected", "-fvisibility-nodllstorageclass=default",
        "-fvisibility-externs-dllimport=default", "-fvisibility-externs-nodllstorageclass=default",
        "-target-cpu", "x86-64-v3", "-ffunction-sections", "-fdata-sections",
        "-isystem", root + "/include/c++/v1", "-isystem", root + "/include",
        "-I", root + "/runtime", "-internal-isystem", root + "/clang-include",
        "-O1", "-Wno-return-type", "-Wno-unused-label", "-std=gnu11", "-fvisibility=hidden",
        "-femulated-tls", "-fgnuc-version=4.2.1", "-fdeclspec", "-faddrsig",
        "-main-file-name", "main_040.c", "-fmacro-prefix-map=" + root + "=/s8",
        "-fdebug-prefix-map=" + root + "=/s8",
        "-D__GCC_HAVE_DWARF2_CFI_ASM=1", "-x", "c", input
    };
    std::vector<const char *> raw;
    for (const auto &a : args) raw.push_back(a.c_str());
    Result warm = compile(raw);
    say("[s8] warmup status=%s compile_ms=%.3f\n", warm.ok ? "ok" : "error", warm.ns / 1e6);
    if (!warm.diagnostics.empty()) say("[s8] warmup diagnostics: %.1500s\n", warm.diagnostics.c_str());
    if (!warm.ok) return finish(1);
    Result single;
    bool ok = run(1, raw, &warm, &single);
    say("[s8] object module=main_040.c input_bytes=%lld bytes=%zu sha256=%s status=%s\n",
        (long long)st.st_size, single.bytes, hex(single.sha).c_str(), single.ok ? "ok" : "error");
    ok = report_blocks(single, root) && ok;
    for (int n : {2, 4, 6, 8}) ok = run(n, raw, &single, nullptr) && ok;
    rusage usage{};
    long rss = getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
    say("[s8] done status=%s peak_rss_kib=%ld\n", ok ? "ok" : "error", rss);
    return finish(ok ? 0 : 1);
}
