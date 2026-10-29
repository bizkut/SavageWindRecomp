// Compile one private generated module in-process for the M12 compiler spike.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef __PROSPERO__
extern "C" int sceSystemServiceLoadExec(const char *, const char *const *);
extern "C" int sceKernelDebugOutText(int, const char *);
#endif

// A title's stdout doesn't reach the console log on the PS5: lines go to the kernel log (as the game's own do).
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

int main(int argc, char **argv) {
#ifdef __PROSPERO__
    const char *base = "/data/savagewind/s8";
#else
    const char *base = argc > 1 ? argv[1] : "build/ps5/s8-input";
#endif
    const std::string root(base), input = root + "/main_040.c";
    struct stat st{};
    say("[s8] start: %s\n", input.c_str());
    if (stat(input.c_str(), &st) != 0) {
        say("[s8] failed: cannot stat %s\n", input.c_str());
        return finish(1);
    }
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
        "-D__GCC_HAVE_DWARF2_CFI_ASM=1", "-x", "c", input
    };
    std::vector<const char *> raw;
    for (const auto &a : args) raw.push_back(a.c_str());
    clang::CompilerInstance compiler;
    std::string diagnostics;
    llvm::raw_string_ostream diag_stream(diagnostics);
    auto diag_options = llvm::makeIntrusiveRefCnt<clang::DiagnosticOptions>();
    compiler.createDiagnostics(new clang::TextDiagnosticPrinter(diag_stream, diag_options.get()));
    if (!compiler.hasDiagnostics()) { say("[s8] failed: no diagnostics engine\n"); return finish(1); }
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*invocation, raw, compiler.getDiagnostics())) {
        diag_stream.flush();
        say("[s8] failed: arguments: %.1500s\n", diagnostics.c_str());
        return finish(1);
    }
    compiler.setInvocation(invocation);
    llvm::SmallVector<char, 0> object;
    compiler.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(object));
    timespec before{}, after{};
    clock_gettime(CLOCK_MONOTONIC, &before);
    clang::EmitObjAction action;
    bool ok = compiler.ExecuteAction(action);
    clock_gettime(CLOCK_MONOTONIC, &after);
    rusage usage{};
    long rss = getrusage(RUSAGE_SELF, &usage) == 0 ? usage.ru_maxrss : -1;
    long long ms = (after.tv_sec - before.tv_sec) * 1000LL + (after.tv_nsec - before.tv_nsec) / 1000000;
    diag_stream.flush();
    if (!diagnostics.empty()) say("[s8] diagnostics: %.1500s\n", diagnostics.c_str());
    say("[s8] summary module main_040.c bytes_in %lld obj_bytes %zu compile_ms %lld peak_rss_kib %ld threads 1 status %s\n",
        static_cast<long long>(st.st_size), object.size(), ms, rss, ok ? "ok" : "error");
    return finish(ok && !object.empty() ? 0 : 1);
}
