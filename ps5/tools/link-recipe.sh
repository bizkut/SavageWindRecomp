#!/usr/bin/env bash
# Savage Wind PS5 - how every title of this port links (sourced by ps5/tools/build-title.sh).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A port of ../forever/PS5_Vulkan/tools/radv-link.sh (Mihawk-99, GPL-3.0-or-later, revision in ps5/deps.lock),
# kept here so this port never writes into the shared tree. Read that file for the reasons behind each line; in short:
#   - the SDK's libc++/libc++abi/libunwind and clang's builtins (__emutls_get_address: the target emulates TLS);
#   - the SDK's platform layer (libps5platform.a) bound to libc's names: its heap in direct memory (libc's private
#     heap runs out), its thread stacks, and the libc functions no system module gives a title;
#   - kernel imports from the title's libkernel, never libkernel_web;
#   - the RADV archive linked whole when one is given (PS5_RADV_ARCHIVE), else nothing of Vulkan.
#
# link_recipe SDK_ROOT BUILD_DIR [RADV_ARCHIVE] sets link_inputs, link_flags and link_stubs.

link_recipe() {
    local sdk_root=$1 build=$2 archive=${3:-}
    local compiler=${PS5_CLANG:-$(command -v clang-18 || command -v clang || true)}
    [[ -n $compiler ]] || { echo "clang was not found; set PS5_CLANG" >&2; return 2; }
    local builtins
    builtins="$("$compiler" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
    local platform="$sdk_root/target/lib/libps5platform.a"
    local lib="$sdk_root/target/lib"

    link_inputs=(-L "$lib")
    [[ -z $archive ]] || link_inputs+=(--whole-archive "$archive" --no-whole-archive)
    link_inputs+=(--start-group "$lib/libc++.a" "$lib/libc++abi.a" "$lib/libunwind.a" "$builtins" "$platform"
        --end-group)

    link_stubs=("$lib/libkernel.so" "$lib/libkernel_sys.so")
    # The payload SDK omits this import facade; never link its body into the title.
    local recipe_root
    recipe_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
    mkdir -p "$build/stubs"
    PS5_PAYLOAD_SDK="$sdk_root" sh "$recipe_root/ps5/tooling/prospero-clang18" \
        -std=c11 -O2 -fPIC -c "$recipe_root/ps5/tooling/ps5-stubs/ime_link_stub.c" \
        -o "$build/stubs/ime.o" || return
    "$sdk_root/bin/prospero-lld" --shared -soname libSceIme.sprx \
        -o "$build/stubs/libSceIme.so" "$build/stubs/ime.o" || return
    link_stubs+=("$build/stubs/libSceIme.so")
    local stub
    for stub in "$lib"/*.so; do
        case ${stub##*/} in
            libkernel.so|libkernel_sys.so|libkernel_web.so|libkernel_stub_weak.so|libc_stub_weak.so|libSceIme.so|libSceImeDialog.so|libSceCommonDialog.so) ;;
            *) link_stubs+=("$stub") ;;
        esac
    done

    link_flags=(--wrap=pthread_create --wrap=pthread_join --wrap=pthread_detach --wrap=fclose --wrap=fflush)
    # Resolve the platform backtrace against the SDK's static libunwind, not
    # the broader libc import stub, whose title runtime lacks this export.
    link_flags+=(--undefined=_Unwind_Backtrace)
    # With RADV: Mesa's weak entry points stay unresolved-weak rather than dynamic (PS5_vkQuake tools/build-title.sh)
    [[ -z $archive ]] || link_flags+=(--no-dynamic-linker -z nodynamic-undefined-weak)
    for name in malloc calloc realloc free posix_memalign aligned_alloc memalign \
            malloc_usable_size reallocf reallocarray getline getdelim; do
        link_flags+=("--wrap=$name")
    done
    for name in qsort_r mkstemps openlog popen pclose open_memstream __xuname __assert \
            __memset_chk regcomp regexec regfree regerror localtime_r newlocale freelocale \
            strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l strtold_l \
            snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l \
            btowc_l wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l \
            wcsnrtombs_l mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l \
            __runes_for_locale catopen catgets catclose backtrace backtrace_symbols_fd \
            __cxa_thread_atexit_impl arc4random arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs \
            futimens clock_nanosleep getaddrinfo freeaddrinfo if_nameindex if_freenameindex \
            opendir fdopendir readdir rewinddir dirfd closedir getcwd nl_langinfo nl_langinfo_l getpwuid_r \
            posix_fallocate access openat unlinkat fchmodat fstatat mkdirat renameat memfd_create; do
        link_flags+=("--defsym=$name=ps5_$name")
    done
    local platform_symbols
    platform_symbols=$("$sdk_root/bin/llvm-nm" --defined-only "$platform" 2>/dev/null || true)
    # (isatty, getnameinfo, gai_strerror: only libScePosixForWebKit's stub has them, and that module gives a title null
    # imports; pacbrew's FFmpeg calls isatty from av_log's default callback)
    for name in localeconv strcasestr mkstemp readlink link symlink isatty getnameinfo gai_strerror; do
        if grep -q " T ps5_$name$" <<<"$platform_symbols"; then
            link_flags+=("--defsym=$name=ps5_$name")
        fi
    done

    # Bound names stay local: the title converter refuses exports
    local map="$build/platform-local.map"
    mkdir -p "$build"
    {
        printf '{\n    local:\n'
        local flag
        for flag in "${link_flags[@]}"; do
            [[ $flag == --defsym=* ]] || continue
            flag=${flag#--defsym=}
            printf '        %s;\n' "${flag%%=*}"
        done
        printf '};\n'
    } > "$map"
    link_flags+=(--version-script "$map")

    local file
    for file in ${archive:+"$archive"} "$platform" "$lib/libc++.a" "$lib/libc++abi.a" "$lib/libunwind.a" \
            "$builtins" "${link_stubs[@]}"; do
        [[ -f $file ]] || { echo "missing $file" >&2; return 2; }
    done
}
