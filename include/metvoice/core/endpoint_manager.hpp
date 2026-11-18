#pragma once

#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/core/asio_backend.hpp"
#include "metvoice/matrix/engine.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace metvoice::core {

class EndpointManager {
public:
    explicit EndpointManager(matrix::MixerEngine& engine);
    ~EndpointManager();

    bool attach_input(size_t strip_idx, const std::string& device_query);
    bool attach_output(size_t bus_idx, const std::string& device_query);

    void detach_input(size_t strip_idx);
    void detach_output(size_t bus_idx);
    bool set_input_enabled(size_t index, bool enabled);
    bool set_output_enabled(size_t index, bool enabled);

    [[nodiscard]] std::string get_input_name(size_t strip_idx) const;
    [[nodiscard]] std::string get_output_name(size_t bus_idx) const;

    [[nodiscard]] std::string serialize_devices_json() const;
    [[nodiscard]] std::string serialize_health_json() const;
    void reset_health();

    [[nodiscard]] uint64_t hotplug_generation() const noexcept {
        return hotplug_generation_.load(std::memory_order_relaxed);
    }

private:
    class NotificationClient;

    bool attach_input_locked(size_t index, const std::string& selection, bool remember_query);
    bool attach_output_locked(size_t index, const std::string& selection, bool remember_query);
    void detach_input_locked(size_t index, bool clear_desired);
    void detach_output_locked(size_t index, bool clear_desired);
    void hotplug_worker_proc();
    void reconcile_endpoints();
    void notify_hotplug(bool device_topology_changed = true);

    matrix::MixerEngine& engine_;
    mutable std::mutex mutex_;

    std::vector<std::unique_ptr<WasapiEndpoint>> capture_endpoints_;
    std::vector<std::unique_ptr<WasapiEndpoint>> render_endpoints_;
    std::vector<std::unique_ptr<AsioEndpoint>> capture_asio_endpoints_;
    std::vector<std::unique_ptr<AsioEndpoint>> render_asio_endpoints_;
    std::vector<std::string> capture_names_;
    std::vector<std::string> render_names_;
    std::vector<std::string> capture_ids_;
    std::vector<std::string> render_ids_;
    std::vector<std::string> desired_input_queries_;
    std::vector<std::string> desired_output_queries_;
    mutable std::deque<AudioDebugEvent> recent_events_;

    std::atomic<uint64_t> hotplug_generation_{0};
    std::atomic<bool> stop_hotplug_{false};
    std::mutex hotplug_mutex_;
    std::condition_variable hotplug_cv_;
    bool hotplug_dirty_{false};
    std::thread hotplug_thread_;
    IMMDeviceEnumerator* hotplug_enumerator_{nullptr};
    NotificationClient* hotplug_client_{nullptr};
};

} // namespace metvoice::core
