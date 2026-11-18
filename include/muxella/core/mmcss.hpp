#pragma once

#include <windows.h>
#include <avrt.h>
#include <string>

namespace muxella::core {

/**
 * @brief RAII wrapper for Windows MMCSS (Multimedia Class Scheduler Service).
 * Registers the calling thread with the "Pro Audio" task group.
 */
class ScopedMmcss {
public:
    explicit ScopedMmcss(const wchar_t* task_name = L"Pro Audio") {
        task_index_ = 0;
        handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index_);
        if (handle_) {
            AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
        } else {
            // Only fall back to Win32 TIME_CRITICAL (15) if MMCSS failed; calling it
            // after AvSetMmThreadPriority clobbers BasePriority 26 back down to 15.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
    }

    ~ScopedMmcss() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
            handle_ = nullptr;
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        }
    }

    // Non-copyable, non-movable
    ScopedMmcss(const ScopedMmcss&) = delete;
    ScopedMmcss& operator=(const ScopedMmcss&) = delete;

    [[nodiscard]] bool is_registered() const noexcept {
        return handle_ != nullptr;
    }

    [[nodiscard]] DWORD task_index() const noexcept {
        return task_index_;
    }

private:
    HANDLE handle_{nullptr};
    DWORD task_index_{0};
};

} // namespace muxella::core
