#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include <cstddef>
#include <cmath>

namespace metvoice::matrix {

/**
 * @brief Routing matrix crosspoint connection between Strip i and Bus j.
 */
struct MatrixNode {
    std::atomic<bool> enabled{false};
    std::atomic<float> send_gain_linear{1.0f};

    MatrixNode() = default;

    void set_enabled(bool en) noexcept {
        enabled.store(en, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled.load(std::memory_order_relaxed);
    }

    void set_gain_db(float db) noexcept {
        send_gain_linear.store(std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
    }

    [[nodiscard]] float gain_linear() const noexcept {
        return send_gain_linear.load(std::memory_order_relaxed);
    }
};

/**
 * @brief N x M Routing Matrix Grid.
 */
class RoutingGrid {
public:
    RoutingGrid(size_t num_strips, size_t num_buses)
        : num_strips_(num_strips), num_buses_(num_buses) {
        nodes_ = std::make_unique<MatrixNode[]>(num_strips * num_buses);
    }

    void set_route(size_t strip_idx, size_t bus_idx, bool enabled, float gain_db = 0.0f) {
        if (strip_idx < num_strips_ && bus_idx < num_buses_) {
            auto& node = nodes_[strip_idx * num_buses_ + bus_idx];
            node.set_enabled(enabled);
            node.set_gain_db(gain_db);
        }
    }

    [[nodiscard]] bool is_routed(size_t strip_idx, size_t bus_idx) const noexcept {
        if (strip_idx < num_strips_ && bus_idx < num_buses_) {
            return nodes_[strip_idx * num_buses_ + bus_idx].is_enabled();
        }
        return false;
    }

    [[nodiscard]] float route_gain(size_t strip_idx, size_t bus_idx) const noexcept {
        if (strip_idx < num_strips_ && bus_idx < num_buses_) {
            return nodes_[strip_idx * num_buses_ + bus_idx].gain_linear();
        }
        return 0.0f;
    }

    [[nodiscard]] size_t strips() const noexcept { return num_strips_; }
    [[nodiscard]] size_t buses() const noexcept { return num_buses_; }

private:
    size_t num_strips_;
    size_t num_buses_;
    std::unique_ptr<MatrixNode[]> nodes_;
};

} // namespace metvoice::matrix
