// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>

// The PS5 /app0 filesystem does not reliably zero the page tail after EOF.
// LLVM's normal mmap-backed MemoryBuffer relies on that zero for its terminator.
// Request a volatile, terminated buffer so LLVM reads the file into owned memory.
class GameCodeFile final : public llvm::vfs::File {
    std::unique_ptr<llvm::vfs::File> file;
public:
    explicit GameCodeFile(std::unique_ptr<llvm::vfs::File> f) : file(std::move(f)) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return file->status(); }
    llvm::ErrorOr<std::string> getName() override { return file->getName(); }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
    getBuffer(const llvm::Twine &name, int64_t size, bool, bool) override {
        return file->getBuffer(name, size, true, true);
    }
    std::error_code close() override { return file->close(); }
};

class GameCodeFileSystem final : public llvm::vfs::ProxyFileSystem {
public:
    explicit GameCodeFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs)
        : ProxyFileSystem(std::move(fs)) {}
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine &path) override {
        auto file = ProxyFileSystem::openFileForRead(path);
        if (!file) return file.getError();
        return std::unique_ptr<llvm::vfs::File>(new GameCodeFile(std::move(*file)));
    }
};
