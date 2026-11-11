#include "metvoice/matrix/engine.hpp"

namespace metvoice::matrix {

namespace {
    size_t compute_strip_target_cushion(size_t max_block_frames, bool virtual_cable) noexcept {
        // Virtual cables see occasional 25-95 ms kernel USB/IP arrival gaps under 100% CPU,
        // so give them extra pre-read cushion inside the 682 ms ring without adding latency to hardware mics.
        const size_t mult = virtual_cable
            ? ((max_block_frames >= 512) ? 9 : 6)
            : ((max_block_frames >= 512) ? 6 : 4);
        return std::max<size_t>(2048, max_block_frames * mult);
    }
}

MixerEngine::MixerEngine(size_t num_strips, size_t num_buses, size_t max_block_frames,
                         float sample_rate)
    : input_connected_(std::make_unique<std::atomic<bool>[]>(num_strips)),
      input_virtual_cable_(std::make_unique<std::atomic<bool>[]>(num_strips)),
      output_connected_(std::make_unique<std::atomic<bool>[]>(num_buses)),
      num_strips_(num_strips),
      num_buses_(num_buses),
      max_block_frames_(max_block_frames),
      sample_rate_(sample_rate),
      routing_(num_strips, num_buses),
      route_ramps_(num_strips * num_buses, dsp::GainRamp(0.0f)),
      strip_primed_(num_strips, false),
      strip_starve_count_(num_strips, 0),
      strip_last_l_(num_strips, 0.0f),
      strip_last_r_(num_strips, 0.0f),
      strip_fade_in_(num_strips, false),
      strip_was_active_(num_strips, false), bus_was_active_(num_buses, false),
      bus_needed_(num_buses, false), bus_receives_(num_buses, false),
      strip_telemetry_(std::make_unique<StripTelemetry[]>(num_strips)),
      bus_telemetry_(std::make_unique<BusTelemetry[]>(num_buses)),
      strip_glitch_detectors_(num_strips),
      bus_glitch_detectors_(num_buses) {

    strips_.reserve(num_strips);
    strip_input_rings_.reserve(num_strips);
    strip_buffers_.reserve(num_strips);

    // 65536 floats (~340ms of 48kHz stereo) ensures plenty of cushion against thread jitter
    constexpr size_t RING_CAPACITY = 65536;
    const size_t target_cushion = compute_strip_target_cushion(max_block_frames, false);

    for (size_t i = 0; i < num_strips; ++i) {
        input_connected_[i].store(false);
        input_virtual_cable_[i].store(false);
        strips_.push_back(std::make_unique<dsp::DspChannelStrip>("Strip " + std::to_string(i + 1), sample_rate));
        strip_input_rings_.push_back(std::make_shared<core::SpscRingBuffer<float>>(RING_CAPACITY));
        strip_buffers_.emplace_back(2, max_block_frames);
        drift_correctors_.push_back(std::make_unique<core::ClockDriftCorrector>(2, target_cushion));
    }

    buses_.reserve(num_buses);
    bus_output_rings_.reserve(num_buses);
    bus_buffers_.reserve(num_buses);

    for (size_t j = 0; j < num_buses; ++j) {
        output_connected_[j].store(false);
        buses_.push_back(std::make_unique<dsp::MasterBus>("Bus " + std::to_string(j + 1), sample_rate));
        bus_output_rings_.push_back(std::make_shared<core::SpscRingBuffer<float>>(RING_CAPACITY));
        bus_buffers_.emplace_back(2, max_block_frames);
    }

    const size_t working_frames = static_cast<size_t>(std::ceil(max_block_frames * 1.25)) + 128;
    temp_interleaved_in_.resize(working_frames * 2, 0.0f);
    temp_interleaved_out_.resize(working_frames * 2, 0.0f);
    drift_in_planar_l_.resize(working_frames, 0.0f);
    drift_in_planar_r_.resize(working_frames, 0.0f);
}

void MixerEngine::set_strip_name(size_t idx, const std::string& name) {
    if (idx < num_strips_) strips_[idx]->set_name(name);
}

void MixerEngine::set_bus_name(size_t idx, const std::string& name) {
    if (idx < num_buses_) buses_[idx]->set_name(name);
}

void MixerEngine::process_block(size_t frames) noexcept {
    DspUpdate update;
    // Bounded work per block; coefficients/state are never edited concurrently.
    for (size_t count = 0; count < 256 && dsp_updates_.read(&update, 1) == 1; ++count) {
        const auto& v = update.values;
        if (update.kind == DspKind::BusEq) {
            auto& b = bus(update.index);
            b.set_eq_low(v[0], v[3]); b.set_eq_mid(v[1], v[4], v[6]); b.set_eq_high(v[2], v[5]);
        } else {
            auto& s = strip(update.index);
            switch (update.kind) {
                case DspKind::StripEq:
                    s.set_eq_low(v[0]); s.set_eq_mid(v[1]); s.set_eq_high(v[2]);
                    s.enable_eq(v[0] != 0 || v[1] != 0 || v[2] != 0); break;
                case DspKind::Compressor: s.set_comp(v[0]); break;
                case DspKind::Gate: s.set_gate(v[0]); break;
                case DspKind::Denoise: s.set_denoise(v[0]); break;
                case DspKind::Reverb: s.set_reverb(v[0]); break;
                default: break;
            }
        }
    }
    const size_t to_process = std::min(frames, max_block_frames_);
    if (to_process == 0) return;

    // Check if any strip has Solo active
    bool any_solo = false;
    for (size_t i = 0; i < num_strips_; ++i) {
        if (strips_[i]->is_enabled() && input_connected_[i].load(std::memory_order_acquire) && strips_[i]->is_solo()) {
            any_solo = true;
            break;
        }
    }

    // 1. Ingest audio into strips with closed-loop drift correction and per-strip DSP
    for (size_t i = 0; i < num_strips_; ++i) {
        auto& buf = strip_buffers_[i];
        buf.set_frame_count(to_process);
        const bool enabled = strips_[i]->is_enabled() && input_connected_[i].load(std::memory_order_acquire);
        if (enabled != strip_was_active_[i]) {
            strips_[i]->reset_processing();
            strip_primed_[i] = false;
            strip_starve_count_[i] = 0;
            strip_last_l_[i] = 0.0f;
            strip_last_r_[i] = 0.0f;
            strip_fade_in_[i] = false;
            strip_was_active_[i] = enabled;
            strip_glitch_detectors_[i].reset();
            for (size_t j = 0; j < num_buses_; ++j) route_ramps_[i * num_buses_ + j] = dsp::GainRamp(0.0f);
        }
        if (!enabled) {
            buf.clear_frames();
            strip_input_rings_[i]->discard(strip_input_rings_[i]->available_read());
            strip_telemetry_[i].drift_ppm.store(0.0, std::memory_order_relaxed);
            continue; // Disabled or disconnected: no resampling, DSP, metering, or routing.
        }

        if (strip_input_rings_[i]) {
            const size_t avail = strip_input_rings_[i]->available_read();
            const bool virtual_cable = input_virtual_cable_[i].load(std::memory_order_acquire);
            const size_t target_cushion = compute_strip_target_cushion(max_block_frames_, virtual_cable);
            drift_correctors_[i]->set_max_ratio_offset(virtual_cable ? 0.001 : 0.005);

            if (!strip_primed_[i]) {
                if (avail >= target_cushion) {
                    strip_primed_[i] = true;
                    strip_fade_in_[i] = true;
                    strip_starve_count_[i] = 0;
                    if (avail > target_cushion) {
                        strip_input_rings_[i]->discard((avail - target_cushion) & ~size_t(1));
                    }
                    drift_correctors_[i]->reset(target_cushion);
                    strip_glitch_detectors_[i].reset();
                }
            }

            if (strip_primed_[i]) {
                // We peek up to req_frames to supply the fractional spline resampler
                const size_t req_frames = static_cast<size_t>(std::ceil(to_process * 1.01)) + 16;
                const size_t max_peek_floats = std::min(avail, req_frames * 2);

                if (avail >= to_process * 2) {
                    if (strip_starve_count_[i] == 0 && !strip_fade_in_[i]) {
                        drift_correctors_[i]->update_feedback(avail);
                    }
                    strip_telemetry_[i].drift_ppm.store(drift_correctors_[i]->drift_ppm(), std::memory_order_relaxed);
                    strip_starve_count_[i] = 0;
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
                        strip_telemetry_[i].truncate_events.fetch_add(1, std::memory_order_relaxed);
                        push_engine_event(core::GlitchStage::StripInput, static_cast<uint8_t>(i),
                                          core::GlitchKind::StripTruncate, static_cast<float>(to_process - generated));
                        // Soft extrapolation / decay on minor jitter underrun instead of click
                        float last_l = (generated > 0) ? out_ptrs[0][generated - 1] : strip_last_l_[i];
                        float last_r = (generated > 0) ? out_ptrs[1][generated - 1] : strip_last_r_[i];
                        for (size_t f = generated; f < to_process; ++f) {
                            last_l *= 0.95f;
                            last_r *= 0.95f;
                            out_ptrs[0][f] = last_l;
                            out_ptrs[1][f] = last_r;
                        }
                    }

                    if (strip_fade_in_[i]) {
                        const size_t fade_frames = std::min<size_t>(to_process, 192);
                        for (size_t f = 0; f < fade_frames; ++f) {
                            const float t = static_cast<float>(f + 1) / static_cast<float>(fade_frames);
                            out_ptrs[0][f] = strip_last_l_[i] * (1.0f - t) + out_ptrs[0][f] * t;
                            out_ptrs[1][f] = strip_last_r_[i] * (1.0f - t) + out_ptrs[1][f] * t;
                        }
                        strip_fade_in_[i] = false;
                    }
                    strip_last_l_[i] = out_ptrs[0][to_process - 1];
                    strip_last_r_[i] = out_ptrs[1][to_process - 1];
                } else {
                    strip_telemetry_[i].starve_events.fetch_add(1, std::memory_order_relaxed);
                    push_engine_event(core::GlitchStage::StripInput, static_cast<uint8_t>(i),
                                      core::GlitchKind::StripStarve, static_cast<float>(avail / 2));
                    // Buffer drained below full block: give grace before unprime to rebuild cushion
                    if (++strip_starve_count_[i] > 2) {
                        strip_primed_[i] = false;
                        strip_telemetry_[i].unprime_events.fetch_add(1, std::memory_order_relaxed);
                        push_engine_event(core::GlitchStage::StripInput, static_cast<uint8_t>(i),
                                          core::GlitchKind::StripUnprime, static_cast<float>(avail / 2));
                    }
                    // Smooth ~4 ms fade-out from last sample rather than a hard jump to silence
                    const size_t fade_frames = std::min<size_t>(to_process, 192);
                    float* ch0 = buf.channel_data(0);
                    float* ch1 = buf.channel_data(1);
                    for (size_t f = 0; f < fade_frames; ++f) {
                        const float g = 1.0f - static_cast<float>(f + 1) / static_cast<float>(fade_frames);
                        ch0[f] = strip_last_l_[i] * g;
                        ch1[f] = strip_last_r_[i] * g;
                    }
                    if (fade_frames < to_process) {
                        std::fill(ch0 + fade_frames, ch0 + to_process, 0.0f);
                        std::fill(ch1 + fade_frames, ch1 + to_process, 0.0f);
                    }
                    strip_last_l_[i] = 0.0f;
                    strip_last_r_[i] = 0.0f;
                    strip_fade_in_[i] = true;
                    strip_glitch_detectors_[i].reset();
                }
            } else {
                buf.clear_frames();
                strip_last_l_[i] = 0.0f;
                strip_last_r_[i] = 0.0f;
            }
        } else {
            buf.clear_frames();
            strip_last_l_[i] = 0.0f;
            strip_last_r_[i] = 0.0f;
        }

        strips_[i]->process(buf, !any_solo || strips_[i]->is_solo());

        if (strip_primed_[i]) {
            const float* ch0 = buf.channel_data(0);
            const float* ch1 = buf.channel_data(1);
            bool logged = false;
            for (size_t f = 0; f < to_process; ++f) {
                core::GlitchKind gk{};
                float gv = 0.0f;
                if (strip_glitch_detectors_[i].check_frame(ch0[f], ch1[f], gk, gv)) {
                    strip_telemetry_[i].dsp_glitches.fetch_add(1, std::memory_order_relaxed);
                    if (!logged) {
                        push_engine_event(core::GlitchStage::StripDsp, static_cast<uint8_t>(i), gk, gv);
                        logged = true;
                    }
                }
            }
        }
    }

    // 2. Clear master buses
    for (size_t j = 0; j < num_buses_; ++j) {
        auto& bus_buf = bus_buffers_[j];
        bus_buf.set_frame_count(to_process);
        bus_buf.clear_frames();
        bus_needed_[j] = buses_[j]->is_enabled() &&
            (output_connected_[j].load(std::memory_order_acquire) || recorder_.records_bus(j));
        bus_receives_[j] = false;
    }

    // 3. Matrix Summing
    for (size_t i = 0; i < num_strips_; ++i) {
        if (!strip_was_active_[i]) continue;
        const float* src_l = strip_buffers_[i].channel_data(0);
        const float* src_r = strip_buffers_[i].channel_data(1);

        for (size_t j = 0; j < num_buses_; ++j) {
            if (!bus_needed_[j]) continue;
            auto& ramp = route_ramps_[i * num_buses_ + j];
            ramp.target(routing_.is_routed(i, j) ? routing_.route_gain(i, j) : 0.0f,
                        static_cast<size_t>(sample_rate_ * 0.010f));
            if (!ramp.silent()) {
                bus_receives_[j] = true;
                float* dst_l = bus_buffers_[j].channel_data(0);
                float* dst_r = bus_buffers_[j].channel_data(1);

                for (size_t f = 0; f < to_process; ++f) {
                    const float send_gain = ramp.next();
                    dst_l[f] += src_l[f] * send_gain;
                    dst_r[f] += src_r[f] * send_gain;
                }
            }
        }
    }

    // 4. Master Bus DSP, Limiting, Metering, and Output dispatch
    for (size_t j = 0; j < num_buses_; ++j) {
        auto& bus_buf = bus_buffers_[j];
        const bool active = bus_needed_[j] && bus_receives_[j];
        if (active != bus_was_active_[j]) {
            buses_[j]->reset_processing();
            bus_glitch_detectors_[j].reset();
            bus_was_active_[j] = active;
        }
        if (active) {
            buses_[j]->process(bus_buf);
            const float* b0 = bus_buf.channel_data(0);
            const float* b1 = bus_buf.channel_data(1);
            bool logged = false;
            for (size_t f = 0; f < to_process; ++f) {
                core::GlitchKind gk{};
                float gv = 0.0f;
                if (bus_glitch_detectors_[j].check_frame(b0[f], b1[f], gk, gv)) {
                    bus_telemetry_[j].bus_glitches.fetch_add(1, std::memory_order_relaxed);
                    if (!logged) {
                        push_engine_event(core::GlitchStage::BusOut, static_cast<uint8_t>(j), gk, gv);
                        logged = true;
                    }
                }
            }
        }
        if (!buses_[j]->is_enabled() || !output_connected_[j].load(std::memory_order_acquire)) continue;
        // Connected outputs still receive digital silence when unused, so idle
        // output streams stay stable without running their DSP.

        bus_buf.interleave_to(temp_interleaved_out_.data(), to_process, 2);

        if (bus_output_rings_[j]) {
            bus_output_rings_[j]->write(temp_interleaved_out_.data(), to_process * 2);
        }
    }
    recorder_.capture(strip_buffers_, bus_buffers_, to_process);
}

void MixerEngine::record_scheduler_tick(double jitter_ms, double process_ms, bool late, bool resync) noexcept {
    scheduler_telemetry_.total_blocks.fetch_add(1, std::memory_order_relaxed);
    if (late) {
        scheduler_telemetry_.late_ticks.fetch_add(1, std::memory_order_relaxed);
        push_engine_event(core::GlitchStage::EngineTimer, 255, core::GlitchKind::LateTick, static_cast<float>(jitter_ms));
    }
    if (resync) {
        scheduler_telemetry_.timer_resyncs.fetch_add(1, std::memory_order_relaxed);
        push_engine_event(core::GlitchStage::EngineTimer, 255, core::GlitchKind::TimerResync, static_cast<float>(jitter_ms));
    }
    double prev_j = scheduler_telemetry_.max_jitter_ms.load(std::memory_order_relaxed);
    while (jitter_ms > prev_j &&
           !scheduler_telemetry_.max_jitter_ms.compare_exchange_weak(prev_j, jitter_ms, std::memory_order_relaxed)) {}
    double prev_p = scheduler_telemetry_.max_process_ms.load(std::memory_order_relaxed);
    while (process_ms > prev_p &&
           !scheduler_telemetry_.max_process_ms.compare_exchange_weak(prev_p, process_ms, std::memory_order_relaxed)) {}
}

void MixerEngine::reset_telemetry() noexcept {
    for (size_t i = 0; i < num_strips_; ++i) {
        strip_telemetry_[i].starve_events.store(0, std::memory_order_relaxed);
        strip_telemetry_[i].unprime_events.store(0, std::memory_order_relaxed);
        strip_telemetry_[i].truncate_events.store(0, std::memory_order_relaxed);
        strip_telemetry_[i].dsp_glitches.store(0, std::memory_order_relaxed);
        strip_input_rings_[i]->reset_metrics();
    }
    for (size_t j = 0; j < num_buses_; ++j) {
        bus_telemetry_[j].bus_glitches.store(0, std::memory_order_relaxed);
        bus_output_rings_[j]->reset_metrics();
    }
    scheduler_telemetry_.total_blocks.store(0, std::memory_order_relaxed);
    scheduler_telemetry_.late_ticks.store(0, std::memory_order_relaxed);
    scheduler_telemetry_.timer_resyncs.store(0, std::memory_order_relaxed);
    scheduler_telemetry_.max_jitter_ms.store(0.0, std::memory_order_relaxed);
    scheduler_telemetry_.max_process_ms.store(0.0, std::memory_order_relaxed);
    engine_debug_events_.reset();
}

} // namespace metvoice::matrix
