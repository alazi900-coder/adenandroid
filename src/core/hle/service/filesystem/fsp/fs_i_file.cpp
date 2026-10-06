// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/errors.h"
#include "core/file_sys/save_trace.h"
#include <algorithm>
#include <array>
#include <fmt/format.h>
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"

namespace Service::FileSystem {

IFile::IFile(Core::System& system_, FileSys::VirtualFile file_, std::string trace_path_)
    : ServiceFramework{system_, "IFile"}, backend{std::make_unique<FileSys::Fsa::IFile>(file_)},
      trace_file{std::move(file_)}, trace_path{std::move(trace_path_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IFile::Read>, "Read"},
        {1, D<&IFile::Write>, "Write"},
        {2, D<&IFile::Flush>, "Flush"},
        {3, D<&IFile::SetSize>, "SetSize"},
        {4, D<&IFile::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
        {6, nullptr, "OperateRangeWithBuffer"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IFile::~IFile() {
    try {
        if (!trace_path.empty() && FileSys::SaveTrace::Recorder::Instance().Active()) {
            const auto size = trace_file->GetSize();
            FileSys::SaveTrace::Recorder::Instance().Record(
                "IFile", "Close", trace_path, trace_file->GetFullPath(), 0, 0,
                size, size, &system);
        }
    } catch (...) {}
}

static std::string TraceHash(const FileSys::VirtualFile& file) {
    if (!file || !file->IsReadable()) return "unavailable";
    constexpr u64 offset = 14695981039346656037ULL;
    constexpr u64 prime = 1099511628211ULL;
    u64 hash = offset;
    std::array<u8, 65536> bytes{};
    const auto size = file->GetSize();
    for (size_t pos = 0; pos < size;) {
        const auto count = file->Read(bytes.data(), std::min(bytes.size(), size - pos), pos);
        if (count == 0) return "unavailable";
        for (size_t i = 0; i < count; ++i) { hash ^= bytes[i]; hash *= prime; }
        pos += count;
    }
    return fmt::format("fnv1a64:{:016x}", hash);
}

Result IFile::Read(
    FileSys::ReadOption option, Out<s64> out_size, s64 offset,
    const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_buffer,
    s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    // Read the data from the Storage backend
    R_RETURN(
        backend->Read(reinterpret_cast<size_t*>(out_size.Get()), offset, out_buffer.data(), size));
}

Result IFile::Write(
    const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer,
    FileSys::WriteOption option, s64 offset, s64 size) {
    LOG_DEBUG(Service_FS, "called, option={}, offset={:#x}, length={}", option.value, offset,
              size);

    const bool tracing = !trace_path.empty() && FileSys::SaveTrace::Recorder::Instance().Active();
    u64 before = 0;
    std::string before_hash = "unavailable";
    if (tracing) {
        try {
            before = trace_file->GetSize();
            if (FileSys::SaveTrace::Recorder::Instance().DeepMode())
                before_hash = TraceHash(trace_file);
        } catch (...) {}
    }
    const auto result = backend->Write(offset, buffer.data(), size, option);
    if (tracing) {
        try {
            const auto after = trace_file->GetSize();
            const auto after_hash = FileSys::SaveTrace::Recorder::Instance().DeepMode()
                                        ? TraceHash(trace_file) : "disabled";
            FileSys::SaveTrace::Recorder::Instance().Record(
                "IFile", "Write", trace_path, trace_file->GetFullPath(), result.raw,
                size, before, after, &system,
                fmt::format("offset={}", offset), 0, 0, 0, before_hash, after_hash);
        } catch (...) {}
    }
    R_RETURN(result);
}

Result IFile::Flush() {
    LOG_DEBUG(Service_FS, "called");

    const auto result = backend->Flush();
    try {
        if (!trace_path.empty() && FileSys::SaveTrace::Recorder::Instance().Active()) {
            const auto size = trace_file->GetSize();
            FileSys::SaveTrace::Recorder::Instance().Record(
                "IFile", "Flush", trace_path, trace_file->GetFullPath(), result.raw, 0,
                size, size, &system);
        }
    } catch (...) {}
    R_RETURN(result);
}

Result IFile::SetSize(s64 size) {
    LOG_DEBUG(Service_FS, "called, size={}", size);

    u64 before = 0;
    try {
        if (!trace_path.empty() && FileSys::SaveTrace::Recorder::Instance().Active())
            before = trace_file->GetSize();
    } catch (...) {}
    const auto result = backend->SetSize(size);
    try {
        if (!trace_path.empty() && FileSys::SaveTrace::Recorder::Instance().Active())
            FileSys::SaveTrace::Recorder::Instance().Record(
                "IFile", "SetSize", trace_path, trace_file->GetFullPath(), result.raw, size,
                before, trace_file->GetSize(), &system);
    } catch (...) {}
    R_RETURN(result);
}

Result IFile::GetSize(Out<s64> out_size) {
    LOG_DEBUG(Service_FS, "called");

    R_RETURN(backend->GetSize(out_size));
}

} // namespace Service::FileSystem
