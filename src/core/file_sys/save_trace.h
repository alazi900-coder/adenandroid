// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_thread.h"

namespace FileSys::SaveTrace {

// Passive observer. Every failure in this module is swallowed so tracing cannot change FS results.
struct Snapshot {
    std::string state{"ARMED"};
    u64 events{};
    u64 writes{};
    u64 bytes{};
    u64 errors{};
};

class Recorder {
public:
    static Recorder& Instance() {
        static Recorder recorder;
        return recorder;
    }

    void Arm(bool deep = false, bool automatic = false) noexcept {
        try {
            std::scoped_lock lock(mutex);
            enabled = true;
            auto_trigger = automatic;
            deep_mode = deep;
            stats = {};
            stats.state = "ARMED";
            ring.clear();
            sequence = 0;
            session_program_id = 0;
            session_user_id_low = 0;
            session_user_id_high = 0;
            host_writes = 0;
            first_error.clear();
            const auto dir = Directory();
            std::filesystem::create_directories(dir);
            Rotate(dir / "trace.jsonl");
            Rotate(dir / "trace.txt");
            std::ofstream(dir / "trace.jsonl", std::ios::trunc);
            std::ofstream(dir / "trace.txt", std::ios::trunc);
        } catch (...) {
            enabled = false;
        }
    }

    void Stop() noexcept {
        try {
            std::scoped_lock lock(mutex);
            if (enabled) {
                stats.state = "CAPTURED";
                WriteReport();
            }
            enabled = false;
            auto_trigger = false;
        } catch (...) {
            enabled = false;
        }
    }

    Snapshot Status() noexcept {
        std::scoped_lock lock(mutex);
        return stats;
    }

    bool DeepMode() const noexcept { return deep_mode; }
    bool Active() const noexcept { return enabled; }

    std::filesystem::path Directory() const {
        return Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "SaveTrace";
    }

    void Record(std::string_view layer, std::string_view operation, std::string_view logical_path,
                std::string_view physical_path, u32 result, u64 size = 0, u64 before = 0,
                u64 after = 0, Core::System* system = nullptr, std::string_view details = {},
                u64 program_id = 0, u64 user_id_low = 0, u64 user_id_high = 0,
                std::string_view hash_before = {}, std::string_view hash_after = {}) noexcept {
        if (!enabled && !(auto_trigger &&
                          (operation == "OpenSaveData" || operation == "CreateSaveData"))) {
            return;
        }
        try {
            if (!enabled) {
                Arm(deep_mode, true);
            }
            std::scoped_lock lock(mutex);
            if (!enabled) return;
            if (program_id != 0) session_program_id = program_id;
            if (user_id_low != 0 || user_id_high != 0) {
                session_user_id_low = user_id_low;
                session_user_id_high = user_id_high;
            }
            if (program_id == 0) program_id = session_program_id;
            if (user_id_low == 0 && user_id_high == 0) {
                user_id_low = session_user_id_low;
                user_id_high = session_user_id_high;
            }
            stats.state = "RECORDING";
            ++stats.events;
            if (operation == "Write" && layer == "IFile") {
                ++stats.writes;
                stats.bytes += size;
            }
            if (operation == "Write" && layer == "RealVfsFile") ++host_writes;
            if (result != 0) ++stats.errors;
            u64 guest_tid = 0, pc = 0, lr = 0;
            if (system != nullptr) {
                if (auto* thread = system->Kernel().GetCurrentEmuThread()) {
                    guest_tid = thread->GetId();
                    if (deep_mode) {
                        const auto& context = thread->GetContext();
                        pc = context.pc;
                        lr = context.lr;
                    }
                }
            }
            const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const auto host_tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            std::ostringstream json;
            json << "{\"seq\":" << ++sequence << ",\"timestamp_ms\":" << stamp
                 << ",\"layer\":\"" << Escape(layer) << "\",\"operation\":\"" << Escape(operation)
                 << "\",\"logical_path\":\"" << Escape(logical_path)
                 << "\",\"physical_path\":\"" << Escape(physical_path)
                 << "\",\"result_code\":" << result << ",\"size\":" << size
                 << ",\"size_before\":" << before << ",\"size_after\":" << after
                 << ",\"host_thread_id\":" << host_tid << ",\"guest_thread_id\":" << guest_tid
                 << ",\"pc\":" << pc << ",\"lr\":" << lr
                 << ",\"program_id\":\"" << std::hex << std::setw(16) << std::setfill('0')
                 << program_id << "\",\"user_id\":\"" << std::setw(16) << user_id_high
                 << std::setw(16) << user_id_low << std::dec
                 << "\",\"save_data_attribute\":\""
                 << Escape(operation == "CreateSaveData" || operation == "OpenSaveData" ? details : "")
                 << "\",\"hash_before\":\"" << Escape(hash_before)
                 << "\",\"hash_after\":\"" << Escape(hash_after)
                 << "\",\"details\":\"" << Escape(details) << "\"}";
            const auto line = json.str();
            if (result != 0 && layer != "SaveDataFactory" && first_error.empty())
                first_error = line;
            ring.push_back(line);
            if (ring.size() > 256) ring.pop_front();
            const auto dir = Directory();
            RotateIfLarge(dir / "trace.jsonl");
            RotateIfLarge(dir / "trace.txt");
            std::ofstream(dir / "trace.jsonl", std::ios::app) << line << '\n';
            std::ofstream(dir / "trace.txt", std::ios::app)
                << stamp << " " << layer << " " << operation << " result=" << result
                << " size=" << size << " before=" << before << " after=" << after
                << " logical=" << logical_path << " physical=" << physical_path
                << " program_id=" << std::hex << program_id << " user_id=" << user_id_high
                << user_id_low << std::dec << " guest_tid=" << guest_tid
                << " pc=" << pc << " lr=" << lr
                << " hash_before=" << hash_before << " hash_after=" << hash_after
                << " details=" << details << '\n';
            // A separate bounded snapshot survives a process crash even if Stop is never called.
            {
                std::ofstream snapshot(dir / "ring.jsonl.tmp", std::ios::trunc);
                for (const auto& entry : ring) snapshot << entry << '\n';
                snapshot.flush();
            }
            std::error_code rename_error;
            std::filesystem::rename(dir / "ring.jsonl.tmp", dir / "ring.jsonl", rename_error);
        } catch (...) {
            // Diagnostics must never alter a guest-visible result.
        }
    }

private:
    static std::string Escape(std::string_view value) {
        std::string out;
        for (char c : value) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: if (static_cast<unsigned char>(c) >= 0x20) out += c;
            }
        }
        return out;
    }
    static void Rotate(const std::filesystem::path& path) {
        if (std::filesystem::exists(path)) {
            const auto old = path.string() + ".old";
            std::error_code ec;
            std::filesystem::remove(old, ec);
            std::filesystem::rename(path, old, ec);
        }
    }
    static void RotateIfLarge(const std::filesystem::path& path) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) &&
            std::filesystem::file_size(path, ec) >= 8 * 1024 * 1024) Rotate(path);
    }
    void WriteReport() {
        std::ofstream report(Directory() / "first_divergence.txt", std::ios::trunc);
        report << "SaveTrace v1 first divergence\n";
        report << "Events: " << stats.events << " Writes: " << stats.writes
               << " Bytes: " << stats.bytes << " Errors: " << stats.errors << "\n";
        if (!first_error.empty()) {
            report << "First observed nonzero result:\n" << first_error << "\n";
        } else if (host_writes < stats.writes) {
            report << "Guest writes: " << stats.writes << ", host writes: " << host_writes
                   << ". First divergence is between IFile and RealVfsFile; inspect trace.jsonl.\n";
        } else {
            report << "No failing result or missing host write observed in captured events.\n";
        }
        report << "Commit is stubbed in Eden IFileSystem; a successful Commit result does not prove persistence.\n";
    }
    std::mutex mutex;
    std::atomic_bool enabled{};
    std::atomic_bool auto_trigger{true};
    std::atomic_bool deep_mode{};
    u64 sequence{};
    u64 session_program_id{};
    u64 session_user_id_low{};
    u64 session_user_id_high{};
    u64 host_writes{};
    std::string first_error;
    Snapshot stats;
    std::deque<std::string> ring;
};

} // namespace FileSys::SaveTrace
