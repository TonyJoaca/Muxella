#include "metvoice/matrix/engine.hpp"

namespace metvoice::matrix {

MixerEngine::MixerEngine(size_t num_strips, size_t num_buses, size_t max_block_frames,
                         float sample_rate)
    : num_strips_(num_strips),
      num_buses_(num_buses),
      max_block_frames_(max_block_frames),
      sample_rate_(sample_rate),
      routing_(num_strips, num_buses),
      strip_primed_(num_strips, false) {

    strips_.reserve(num_strips);
    strip_input_rings_.reserve(num_strips);
    strip_buffers_.reserve(num_strips);

    // 65536 floats (~340ms of 48kHz stereo) ensures plenty of cushion against thread jitter
    constexpr size_t RING_CAPACITY = 65536;

    for (size_t i = 0; i < num_strips; ++i) {
        strips_.push_back(std::make_unique<dsp::DspChannelStrip>("Strip " + std::to_string(i + 1), sample_rate));
        strip_input_rings_.push_back(std::make_shared<core::SpscRingBuffer<float>>(RING_CAPACITY));
        strip_buffers_.emplace_back(2, max_block_frames);
    }

    buses_.reserve(num_buses);
    bus_output_rings_.reserve(num_buses);
    bus_buffers_.reserve(num_buses);

    for (size_t j = 0; j < num_buses; ++j) {
        buses_.push_back(std::make_unique<dsp::MasterBus>("Bus " + std::to_string(j + 1), sample_rate));
        bus_output_rings_.push_back(std::make_shared<core::SpscRingBuffer<float>>(RING_CAPACITY));
        bus_buffers_.emplace_back(2, max_block_frames);
    }

    temp_interleaved_in_.resize(max_block_frames * 2, 0.0f);
    temp_interleaved_out_.resize(max_block_frames * 2, 0.0f);
}

void MixerEngine::set_strip_name(size_t idx, const std::string& name) {
    if (idx < num_strips_) strips_[idx]->set_name(name);
}

void MixerEngine::set_bus_name(size_t idx, const std::string& name) {
    if (idx < num_buses_) buses_[idx]->set_name(name);
}

void MixerEngine::process_block(size_t frames) noexcept {
    const size_t to_process = std::min(frames, max_block_frames_);
    if (to_process == 0) return;

    // Check if any strip has Solo active
    bool any_solo = false;
    for (size_t i = 0; i < num_strips_; ++i) {
        if (strips_[i]->is_solo()) {
            any_solo = true;
            break;
        }
    }

    // 1. Ingest audio into strips with jitter cushion and apply per-strip DSP
    for (size_t i = 0; i < num_strips_; ++i) {
        auto& buf = strip_buffers_[i];
        buf.set_frame_count(to_process);

        const size_t samples_needed = to_process * 2;
        size_t samples_read = 0;

        if (strip_input_rings_[i]) {
            const size_t avail = strip_input_rings_[i]->available_read();
            // Cushion threshold: at least 2 full blocks (e.g. 1920 floats = 20ms) before starting playback
            const size_t cushion_threshold = samples_needed * 2;

            if (!strip_primed_[i]) {
                if (avail >= cushion_threshold) {
                    strip_primed_[i] = true;
                }
            }

            if (strip_primed_[i]) {
                if (avail >= samples_needed) {
                    samples_read = strip_input_rings_[i]->read(temp_interleaved_in_.data(), samples_needed);

                    // Prevent excessive latency buildup (keep buffer under ~60ms)
                    if (avail > samples_needed * 6) {
                        size_t excess = std::min(avail - samples_needed * 2, samples_needed);
                        // Read and discard excess to re-align
                        strip_input_rings_[i]->read(temp_interleaved_out_.data(), excess);
                    }
                } else {
                    // Starvation detected: drop to silence and wait for cushion to reaccumulate
                    strip_primed_[i] = false;
                    samples_read = 0;
                }
            }
        }

        // Fill remaining with silence if underrun or unprimed
        if (samples_read < samples_needed) {
            std::fill(temp_interleaved_in_.data() + samples_read,
                      temp_interleaved_in_.data() + samples_needed, 0.0f);
        }

        buf.deinterleave_from(temp_interleaved_in_.data(), to_process, 2);
        strips_[i]->process(buf);
    }

    // 2. Clear master buses
    for (size_t j = 0; j < num_buses_; ++j) {
        auto& bus_buf = bus_buffers_[j];
        bus_buf.set_frame_count(to_process);
        bus_buf.clear_frames();
    }

    // 3. Matrix Summing
    for (size_t i = 0; i < num_strips_; ++i) {
        // Solo & Mute arbitration
        if (any_solo && !strips_[i]->is_solo()) {
            continue; // Muted by solo of another channel
        }
        if (strips_[i]->is_muted()) {
            continue;
        }

        const float* src_l = strip_buffers_[i].channel_data(0);
        const float* src_r = strip_buffers_[i].channel_data(1);

        for (size_t j = 0; j < num_buses_; ++j) {
            if (routing_.is_routed(i, j)) {
                const float send_gain = routing_.route_gain(i, j);
                float* dst_l = bus_buffers_[j].channel_data(0);
                float* dst_r = bus_buffers_[j].channel_data(1);

                for (size_t f = 0; f < to_process; ++f) {
                    dst_l[f] += src_l[f] * send_gain;
                    dst_r[f] += src_r[f] * send_gain;
                }
            }
        }
    }

    // 4. Master Bus DSP, Limiting, Metering, and Output dispatch
    for (size_t j = 0; j < num_buses_; ++j) {
        auto& bus_buf = bus_buffers_[j];
        buses_[j]->process(bus_buf);

        bus_buf.interleave_to(temp_interleaved_out_.data(), to_process, 2);

        if (bus_output_rings_[j]) {
            bus_output_rings_[j]->write(temp_interleaved_out_.data(), to_process * 2);
        }
    }
}

} // namespace metvoice::matrix
