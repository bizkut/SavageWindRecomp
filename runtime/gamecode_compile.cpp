// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamecode_compile.h"
#include <string>
void s9_output_sink(const std::string &, const std::string &);
#define S9_OUTPUT_SINK
#define main gamecode_generator_main
#include "../tools/gen_cpp/emit.cpp"
#undef main

#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <pthread.h>
#ifdef __PROSPERO__
extern "C" int sceKernelDebugOutText(int, const char *);
#endif
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace {
using Digest = std::array<uint8_t, 32>;
struct Job {
    std::string name;
    std::string source;
};
struct Object {
    std::string name;
    std::vector<char> data;
};
struct Result {
    std::vector<Object> storage;
    std::vector<GameLinkObject> objects;
};

static void add_field(llvm::SHA256 &hash, llvm::StringRef value)
{
    uint8_t len[8];
    uint64_t size = value.size();
    for (unsigned i = 0; i < 8; ++i)
        len[i] = static_cast<uint8_t>(size >> (i * 8));
    hash.update(llvm::ArrayRef<uint8_t>(len, sizeof len));
    hash.update(value);
}

static Digest header_digest(const fs::path &root)
{
    std::vector<fs::path> paths;
    for (const char *name : {"runtime", "include", "clang-include"}) {
        fs::path dir = root / name;
        require(fs::is_directory(dir), "missing include directory");
        for (const auto &entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file())
                paths.push_back(entry.path().lexically_relative(root));
            else
                require(entry.is_directory(), "unsupported include tree entry");
        }
    }
    std::sort(paths.begin(), paths.end());
    llvm::SHA256 hash;
    for (const auto &path : paths) {
        add_field(hash, path.generic_string());
        Bytes content = file(root / path);
        add_field(hash, llvm::StringRef(reinterpret_cast<const char *>(content.data()), content.size()));
    }
    return hash.final();
}

static std::vector<std::string> arguments(const std::string &root, const std::string &name)
{
    return {"-triple", "x86_64-sie-ps5", "-emit-obj", "-disable-free", "-clear-ast-before-backend",
        "-disable-llvm-verifier", "-discard-value-names", "-main-file-name", name,
        "-mrelocation-model", "pic", "-pic-level", "2", "-fhalf-no-semantic-interposition",
        "-mframe-pointer=non-leaf", "-relaxed-aliasing", "-fdenormal-fp-math=preserve-sign,preserve-sign",
        "-ffp-contract=on", "-fno-rounding-math", "-mconstructor-aliases", "-fno-plt", "-funwind-tables=2",
        "-fno-use-init-array", "-fvisibility-global-new-delete=source", "-fvisibility-from-dllstorageclass",
        "-fvisibility-dllexport=protected", "-fvisibility-nodllstorageclass=default",
        "-fvisibility-externs-dllimport=default", "-fvisibility-externs-nodllstorageclass=default",
        "-target-cpu", "x86-64-v3", "-gno-column-info", "-debugger-tuning=sce",
        "-mllvm", "-generate-arange-section", "-debug-forward-template-params", "-dwarf-explicit-import",
        "-fdebug-compilation-dir=/gamecode", "-ffunction-sections", "-fdata-sections",
        "-fcoverage-compilation-dir=/gamecode", "-resource-dir", root,
        "-isysroot", root, "-isystem", root + "/include/c++/v1", "-isystem", root + "/include",
        "-I", root + "/runtime", "-internal-isystem", root + "/clang-include",
        "-internal-externc-isystem", root + "/include", "-internal-externc-isystem", root + "/include_common",
        "-O1", "-Wno-return-type", "-Wno-unused-label", "-std=gnu11", "-ferror-limit", "19",
        "-fvisibility=hidden", "-femulated-tls", "-fgnuc-version=4.2.1", "-fdeclspec",
        "-fskip-odr-check-in-gmf", "-faddrsig", "-D__GCC_HAVE_DWARF2_CFI_ASM=1",
        "-x", "c", "/gamecode/" + name};
}

static Digest cache_key(const Job &job, const Digest &headers, const Digest &llvm_build)
{
    llvm::SHA256 hash;
    add_field(hash, "gamecode-object-v2");
    add_field(hash, LLVM_VERSION_STRING);
    add_field(hash, llvm::StringRef(reinterpret_cast<const char *>(headers.data()), headers.size()));
    add_field(hash, llvm::StringRef(reinterpret_cast<const char *>(llvm_build.data()), llvm_build.size()));
    for (const auto &arg : arguments("/include-root", job.name))
        add_field(hash, arg);
    add_field(hash, job.source);
    return hash.final();
}

static std::vector<char> compile_object(const Job &job, const std::string &root)
{
    auto args = arguments(root, job.name);
    std::vector<const char *> raw;
    for (const auto &arg : args)
        raw.push_back(arg.c_str());
    std::string diagnostics;
    llvm::raw_string_ostream diag_stream(diagnostics);
    auto diag_options = llvm::makeIntrusiveRefCnt<clang::DiagnosticOptions>();
    clang::CompilerInstance compiler;
    compiler.createDiagnostics(new clang::TextDiagnosticPrinter(diag_stream, diag_options.get()));
    require(compiler.hasDiagnostics(), "cannot create compiler diagnostics");
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*invocation, raw, compiler.getDiagnostics())) {
        diag_stream.flush();
        throw std::runtime_error(job.name + ": " + diagnostics);
    }
    // The driver exits after one file; its -disable-free would leak every AST in a title.
    // Match Clang Tooling's in-process ownership instead.
    invocation->getFrontendOpts().DisableFree = false;
    invocation->getCodeGenOpts().DisableFree = false;
    invocation->getCodeGenOpts().ClearASTBeforeBackend = false;
    require(invocation->getCodeGenOpts().DebugPass.empty() &&
            invocation->getCodeGenOpts().LimitFloatPrecision.empty(), "unsupported global LLVM backend options");
    invocation->getPreprocessorOpts().addRemappedFile("/gamecode/" + job.name,
        llvm::MemoryBuffer::getMemBufferCopy(job.source, "/gamecode/" + job.name).release());
    compiler.setInvocation(invocation);
    llvm::SmallVector<char, 0> data;
    compiler.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(data));
    clang::EmitObjAction action;
#ifdef __PROSPERO__
    // Written straight to the kernel log (not the asynchronous logger), so a crash inside the compiler names its file.
    std::string line = "savagewind: [gamecode] compile start " + job.name + "\n";
    sceKernelDebugOutText(0, line.c_str());
#endif
    bool ok = compiler.ExecuteAction(action);
    diag_stream.flush();
    if (!ok || data.empty())
        throw std::runtime_error(job.name + ": " + diagnostics);
    return std::vector<char>(data.begin(), data.end());
}

struct Build {
    std::string root;
    Digest headers, llvm_build;
    const GameCodeObjectCache *cache;
    GameCodeProgress progress;
    void *progress_context;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Job> queue;
    std::vector<Object> result;
    std::string error;
    size_t completed = 0;
    size_t submitted = 0;
    size_t capacity;
    bool finished = false;

    void report(const char *stage, size_t done, size_t total) {
        if (progress)
            progress(progress_context, stage, done, total);
    }
    void enqueue(const std::string &path, const std::string &source) {
        if (fs::path(path).extension() != ".c")
            return;
        std::string name = fs::path(path).filename().string();
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return queue.size() < capacity || !error.empty(); });
        if (!error.empty())
            throw std::runtime_error(error);
        queue.push_back({name, source});
        ++submitted;
        changed.notify_all();
    }
    void worker() {
        try {
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return !queue.empty() || finished || !error.empty(); });
                    if (!error.empty() || (queue.empty() && finished))
                        return;
                    job = std::move(queue.front());
                    queue.pop_front();
                    changed.notify_all();
                }
                Digest key = cache_key(job, headers, llvm_build);
                std::vector<char> data;
                bool hit = false;
                if (cache) {
                    std::lock_guard<std::mutex> lock(mutex);
                    const void *cached = nullptr;
                    size_t size = 0;
                    int status = cache->lookup(cache->context, key.data(), &cached, &size);
                    if (status < 0 || status > 1 || (status == 1 && (!cached || !size)))
                        throw std::runtime_error("object cache lookup failed");
                    if (status == 1) {
                        const char *p = static_cast<const char *>(cached);
                        try {
                            data.assign(p, p + size);
                        } catch (...) {
                            cache->release(cache->context, cached, size);
                            throw;
                        }
                        cache->release(cache->context, cached, size);
                        hit = true;
                    }
                }
                if (!hit)
                    data = compile_object(job, root);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!hit && cache && cache->store(cache->context, key.data(), data.data(), data.size()))
                        throw std::runtime_error("object cache store failed");
                    job.name.replace(job.name.size() - 2, 2, ".o");
                    result.push_back({std::move(job.name), std::move(data)});
                    ++completed;
                    report(hit ? "cache" : "compile", completed, finished ? submitted : 0);
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty())
                error = e.what();
            changed.notify_all();
        }
    }
};

// The generator is synchronous; simultaneous builds would race this sink.
static std::mutex build_mutex;
static Build *active_build;
}

void s9_output_sink(const std::string &filename, const std::string &text)
{
    active_build->enqueue(filename, text);
}

extern "C" int gamecode_build_objects(const GameCodeCompileInputs *inputs, const char *include_root,
                                       unsigned threads, const GameCodeObjectCache *cache,
                                       GameCodeProgress progress, void *progress_context,
                                       GameCodeObjects *out, char *error, size_t error_size)
{
    if (out)
        *out = {};
    if (error && error_size)
        error[0] = 0;
    try {
        require(inputs && include_root && *include_root && out && threads, "invalid compile arguments");
        require(!cache || (cache->lookup && cache->release && cache->store), "invalid cache callbacks");
        for (const auto &module : inputs->modules)
            require(module.data && module.size, "missing module input");
        require(inputs->hle.data || !inputs->hle.size, "invalid HLE input");
        require(inputs->layout.data || !inputs->layout.size, "invalid layout input");
        require(LLVM_ENABLE_THREADS, "LLVM threads disabled");
        std::lock_guard<std::mutex> build_lock(build_mutex);
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            LLVMInitializeX86TargetInfo();
            LLVMInitializeX86Target();
            LLVMInitializeX86TargetMC();
            LLVMInitializeX86AsmPrinter();
            LLVMInitializeX86AsmParser();
            const char *llvm_args[] = {"gamecode", "-generate-arange-section"};
            llvm::cl::ParseCommandLineOptions(2, llvm_args);
        });
        Build build;
        build.root = fs::absolute(include_root).lexically_normal().string();
        build.headers = header_digest(build.root);
        std::copy(inputs->llvm_build_identity, inputs->llvm_build_identity + 32, build.llvm_build.begin());
        build.cache = cache;
        build.progress = progress;
        build.progress_context = progress_context;
        build.capacity = std::max<size_t>(threads * 2u, 1);
        const char *names[] = {"main", "subsdk0", "sdk"};
        std::vector<Bytes> modules;
        std::vector<Layout> layouts;
        U base = 0x7000000000ULL;
        for (unsigned i = 0; i < 3; ++i) {
            const auto *p = static_cast<const uint8_t *>(inputs->modules[i].data);
            modules.emplace_back(p, p + inputs->modules[i].size);
            Header h(modules.back());
            U size = h.image_size();
            require(size <= 0x7080000000ULL - base, "layout exceeds module region");
            layouts.push_back({names[i], base, size, h.id});
            base += size;
        }
        std::string layout_text =
            "# module layout (tools/layout.py), load order: name, guest base, image size, NSO build id\n";
        for (const auto &module : layouts)
            layout_text += module.name + " " + hex_plain(module.base) + " " + hex_plain(module.size) + " " +
                           module.id + "\n";
        if (inputs->layout.data)
            require(layout_text == std::string(static_cast<const char *>(inputs->layout.data), inputs->layout.size),
                    "layout mismatch");
        auto sites = [&] {
            Nso main_nso(modules[0]);
            return game_sites::for_module(main_nso, layouts[0].base, false);
        }();
        std::string hle;
        if (inputs->hle.size)
            hle.assign(static_cast<const char *>(inputs->hle.data), inputs->hle.size);
        std::vector<pthread_t> workers;
        workers.reserve(threads);
        active_build = &build;
        try {
            pthread_attr_t attributes;
            int rc = pthread_attr_init(&attributes);
            require(!rc, "cannot initialize worker attributes");
            rc = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
            if (!rc) {
                for (unsigned i = 0; i < threads; ++i) {
                    pthread_t worker;
                    rc = pthread_create(&worker, &attributes, [](void *context) -> void * {
                        static_cast<Build *>(context)->worker();
                        return nullptr;
                    }, &build);
                    if (rc)
                        break;
                    workers.push_back(worker);
                }
            }
            pthread_attr_destroy(&attributes);
            require(!rc, "cannot create compiler worker");
            write_layout(layouts, ".", sites.version, sites.sites);
            Options options;
            for (unsigned i = 0; i < 3; ++i) {
                {
                    std::lock_guard<std::mutex> lock(build.mutex);
                    build.report("generate", i, 3);
                }
                generate(names[i], {}, layouts[i].base, fs::path(".") / names[i], options,
                         &sites.sites, &modules[i], i ? &hle : nullptr);
            }
            {
                std::lock_guard<std::mutex> lock(build.mutex);
                build.report("generate", 3, 3);
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(build.mutex);
            if (build.error.empty())
                build.error = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(build.mutex);
            build.finished = true;
            build.changed.notify_all();
        }
        for (auto worker : workers)
            pthread_join(worker, nullptr);
        active_build = nullptr;
        if (!build.error.empty())
            throw std::runtime_error(build.error);
        std::sort(build.result.begin(), build.result.end(), [](const Object &a, const Object &b) {
            return a.name < b.name;
        });
        auto result = std::make_unique<Result>();
        result->storage = std::move(build.result);
        result->objects.reserve(result->storage.size());
        for (const auto &object : result->storage)
            result->objects.push_back({object.data.data(), object.data.size(), object.name.c_str()});
        out->objects = result->objects.data();
        out->count = result->objects.size();
        out->private_data = result.release();
        return 0;
    } catch (const std::exception &e) {
        if (error && error_size)
            std::snprintf(error, error_size, "%s", e.what());
        return -1;
    }
}

extern "C" void gamecode_free_objects(GameCodeObjects *objects)
{
    if (objects) {
        delete static_cast<Result *>(objects->private_data);
        *objects = {};
    }
}
