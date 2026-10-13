#pragma once

#include <windows.h>
#include <avrt.h>
#include <string>

namespace metvoice::core {

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
        }
    }

    ~ScopedMmcss() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
            handle_ = nullptr;
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

} // namespace metvoice::core
