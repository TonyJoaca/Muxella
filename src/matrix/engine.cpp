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
        drift_correctors_.push_back(std::make_unique<core::ClockDriftCorrector>(2, 2048));
    }

    buses_.reserve(num_buses);
    bus_output_rings_.reserve(num_buses);
    bus_buffers_.reserve(num_buses);

    for (size_t j = 0; j < num_buses; ++j) {
        buses_.push_back(std::make_unique<dsp::MasterBus>("Bus " + std::to_string(j + 1), sample_rate));
        bus_output_rings_.push_back(std::make_shared<core::SpscRingBuffer<float>>(RING_CAPACITY));
        bus_buffers_.emplace_back(2, max_block_frames);
    }

    temp_interleaved_in_.resize((max_block_frames + 64) * 2, 0.0f);
    temp_interleaved_out_.resize((max_block_frames + 64) * 2, 0.0f);
    drift_in_planar_l_.resize(max_block_frames + 64, 0.0f);
    drift_in_planar_r_.resize(max_block_frames + 64, 0.0f);
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

    // 1. Ingest audio into strips with closed-loop drift correction and per-strip DSP
    for (size_t i = 0; i < num_strips_; ++i) {
        auto& buf = strip_buffers_[i];
        buf.set_frame_count(to_process);

        if (strip_input_rings_[i]) {
            const size_t avail = strip_input_rings_[i]->available_read();
            constexpr size_t TARGET_CUSHION = 2048; // ~21ms at 48kHz stereo cushion

            if (!strip_primed_[i]) {
                if (avail >= TARGET_CUSHION) {
                    strip_primed_[i] = true;
                    drift_correctors_[i]->reset(TARGET_CUSHION);
                }
            }

            if (strip_primed_[i]) {
                drift_correctors_[i]->update_feedback(avail);

                // We peek up to (to_process + 16) frames to supply the fractional spline resampler
                const size_t req_frames = to_process + 16;
                const size_t max_peek_floats = std::min(avail, req_frames * 2);

                if (max_peek_floats >= 8) {
                    const size_t peeked_floats = strip_input_rings_[i]->peek(temp_interleaved_in_.data(), max_peek_floats);
                    const size_t in_frames = peeked_floats / 2;

                    for (size_t f = 0; f < in_frames; ++f) {
                        drift_in_planar_l_[f] = temp_interleaved_in_[f * 2 + 0];
                        drift_in_planar_r_[f] = temp_interleaved_in_[f * 2 + 1];
                    }

                    const float* in_ptrs[2] = {drift_in_planar_l_.data(), drift_in_planar_r_.data()};
                    float* out_ptrs[2] = {buf.channel_data(0), buf.channel_data(1)};

                    size_t frames_consumed = 0;
                    size_t generated = drift_correctors_[i]->process(in_ptrs, in_frames, out_ptrs, to_process, frames_consumed);

                    // Advance ring buffer strictly by consumed frames
                    strip_input_rings_[i]->discard(frames_consumed * 2);

                    if (generated < to_process) {
                        // Soft extrapolation / decay on minor jitter underrun instead of click
                        float last_l = (generated > 0) ? out_ptrs[0][generated - 1] : 0.0f;
                        float last_r = (generated > 0) ? out_ptrs[1][generated - 1] : 0.0f;
                        for (size_t f = generated; f < to_process; ++f) {
                            last_l *= 0.95f;
                            last_r *= 0.95f;
                            out_ptrs[0][f] = last_l;
                            out_ptrs[1][f] = last_r;
                        }
                    }
                } else {
                    // Buffer drained: unprime and softly clear
                    strip_primed_[i] = false;
                    buf.clear_frames();
                }
            } else {
                buf.clear_frames();
            }
        } else {
            buf.clear_frames();
        }

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
