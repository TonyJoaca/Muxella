#include "muxella/usbip/uac1_device.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace muxella::usbip {

namespace {

std::vector<uint8_t> build_string_descriptor(const std::string& ascii) {
    // All product/manufacturer/serial strings are ASCII; encode directly to UTF-16LE.
    std::vector<uint8_t> utf16;
    const size_t max_chars = std::min<size_t>(ascii.size(), 126);
    utf16.resize(2 + max_chars * 2);
    utf16[0] = static_cast<uint8_t>(utf16.size());
    utf16[1] = kDescString;
    for (size_t i = 0; i < max_chars; ++i) {
        utf16[2 + i * 2] = static_cast<uint8_t>(ascii[i]);
        utf16[2 + i * 2 + 1] = 0;
    }
    return utf16;
}

std::vector<uint8_t> build_device_descriptor(int cable_number) {
    const uint16_t pid = static_cast<uint16_t>(0xCB00 + cable_number);
    std::vector<uint8_t> b(18, 0);
    b[0] = 18;
    b[1] = kDescDevice;
    b[2] = 0x10;
    b[3] = 0x01; // USB 1.1 full-speed
    b[4] = 0x00;
    b[5] = 0x00;
    b[6] = 0x00;
    b[7] = 64;   // EP0 packet size
    b[8] = 0xFF; // 0xFFFF VID
    b[9] = 0xFF;
    b[10] = static_cast<uint8_t>(pid & 0xFF);
    b[11] = static_cast<uint8_t>((pid >> 8) & 0xFF);
    b[12] = 0x01;
    b[13] = 0x00;
    b[14] = 1;
    b[15] = 2;
    b[16] = 3;
    b[17] = 1;
    return b;
}

std::vector<uint8_t> build_config_descriptor(int pcm_bits, int pcm_bytes) {
    std::vector<uint8_t> b;
    b.reserve(200);
    auto append = [&](std::initializer_list<uint8_t> xs) {
        b.insert(b.end(), xs.begin(), xs.end());
    };

    const uint16_t max_packet = static_cast<uint16_t>((kUacSampleRate / 1000) * kUacChannels * pcm_bytes);
    const uint8_t mp_lo = static_cast<uint8_t>(max_packet & 0xFF);
    const uint8_t mp_hi = static_cast<uint8_t>((max_packet >> 8) & 0xFF);
    const uint8_t sub_bytes = static_cast<uint8_t>(pcm_bytes);
    const uint8_t bit_res = static_cast<uint8_t>(pcm_bits);

    // Configuration header; wTotalLength patched below.
    append({9, kDescConfiguration, 0, 0, 3, 1, 0, 0x80, 50});

    // AudioControl interface 0
    append({9, kDescInterface, 0, 0, 0, 0x01, 0x01, 0x00, 0});
    append({10, kDescCsInterface, 0x01, 0x00, 0x01, 72, 0, 2, 1, 2});
    // Playback path
    append({12, kDescCsInterface, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0});
    append({10, kDescCsInterface, 0x06, 2, 1, 1, 0x03, 0x00, 0x00, 0});
    append({9, kDescCsInterface, 0x03, 3, 0x01, 0x03, 0, 2, 0});
    // Capture path
    append({12, kDescCsInterface, 0x02, 4, 0x01, 0x02, 0, 2, 0x03, 0x00, 0, 0});
    append({10, kDescCsInterface, 0x06, 5, 4, 1, 0x03, 0x00, 0x00, 0});
    append({9, kDescCsInterface, 0x03, 6, 0x01, 0x01, 0, 5, 0});

    // Playback AudioStreaming interface 1, alt 0 & alt 1 (EP 0x01 OUT)
    append({9, kDescInterface, 1, 0, 0, 0x01, 0x02, 0x00, 0});
    append({9, kDescInterface, 1, 1, 1, 0x01, 0x02, 0x00, 0});
    append({7, kDescCsInterface, 0x01, 1, 1, 0x01, 0x00});
    append({11, kDescCsInterface, 0x02, 1, 2, sub_bytes, bit_res, 1, 0x80, 0xBB, 0x00});
    append({9, kDescEndpoint, 0x01, 0x09, mp_lo, mp_hi, 1, 0, 0});
    append({7, kDescCsEndpoint, 0x01, 0, 0, 0, 0});

    // Capture AudioStreaming interface 2, alt 0 & alt 1 (EP 0x82 IN)
    append({9, kDescInterface, 2, 0, 0, 0x01, 0x02, 0x00, 0});
    append({9, kDescInterface, 2, 1, 1, 0x01, 0x02, 0x00, 0});
    append({7, kDescCsInterface, 0x01, 6, 1, 0x01, 0x00});
    append({11, kDescCsInterface, 0x02, 1, 2, sub_bytes, bit_res, 1, 0x80, 0xBB, 0x00});
    append({9, kDescEndpoint, 0x82, 0x0D, mp_lo, mp_hi, 1, 0, 0});
    append({7, kDescCsEndpoint, 0x01, 0, 0, 0, 0});

    const uint16_t total = static_cast<uint16_t>(b.size());
    b[2] = static_cast<uint8_t>(total & 0xFF);
    b[3] = static_cast<uint8_t>((total >> 8) & 0xFF);
    return b;
}

void truncate_vec(std::vector<uint8_t>& v, uint16_t max_len) {
    if (v.size() > max_len) {
        v.resize(max_len);
    }
}

void apply_pcm_gain(uint8_t* p, size_t len, bool mute, int16_t volume, int pcm_bits, int pcm_bytes) noexcept {
    if (mute) {
        std::memset(p, 0, len);
        return;
    }
    if (volume == 0) {
        return;
    }
    if (volume > 0) {
        volume = 0;
    }
    const double gain = std::pow(10.0, static_cast<double>(volume) / (256.0 * 20.0));
    const unsigned shift = static_cast<unsigned>(32 - pcm_bits);
    for (size_t offset = 0; offset + static_cast<size_t>(pcm_bytes) <= len; offset += static_cast<size_t>(pcm_bytes)) {
        uint32_t value = 0;
        for (int b = 0; b < pcm_bytes; ++b) {
            value |= static_cast<uint32_t>(p[offset + b]) << (8 * b);
        }
        const int32_t sample = static_cast<int32_t>(value << shift) >> shift;
        const int32_t output = static_cast<int32_t>(std::llround(static_cast<double>(sample) * gain));
        const uint32_t out_u = static_cast<uint32_t>(output);
        for (int b = 0; b < pcm_bytes; ++b) {
            p[offset + b] = static_cast<uint8_t>(out_u >> (8 * b));
        }
    }
}

} // namespace

bool parse_setup(const uint8_t* data, size_t len, SetupPacket& out) noexcept {
    if (!data || len < 8) return false;
    out.request_type = data[0];
    out.request = data[1];
    out.value = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
    out.index = static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8);
    out.length = static_cast<uint16_t>(data[6]) | (static_cast<uint16_t>(data[7]) << 8);
    return true;
}

bool Uac1Descriptors::create(int cable_number, int pcm_bits, Uac1Descriptors& out, std::string* err) {
    if (cable_number < 1 || cable_number > 255) {
        if (err) *err = "cable number must be 1..255";
        return false;
    }
    if (pcm_bits != 16 && pcm_bits != 24 && pcm_bits != 32) {
        if (err) *err = "pcm_bits must be 16, 24, or 32";
        return false;
    }

    static constexpr const char* kNames[] = {
        "Muxella Main", "Muxella Spotify", "Muxella Discord",
        "Muxella Out B1", "Muxella Out B2", "Muxella Out B3"
    };
    char buf[64]{};
    if (cable_number <= 6) {
        out.product = kNames[cable_number - 1];
    } else {
        std::snprintf(buf, sizeof(buf), "Muxella Cable %02d", cable_number);
        out.product = buf;
    }
    std::snprintf(buf, sizeof(buf), "MUXPCM-%03d", cable_number);
    out.serial = buf;
    out.cable_number = cable_number;
    out.pcm_bits = pcm_bits;
    out.pcm_bytes = pcm_bits / 8;

    out.device = build_device_descriptor(cable_number);
    out.config = build_config_descriptor(out.pcm_bits, out.pcm_bytes);
    out.strings[0] = {4, kDescString, 0x09, 0x04};
    out.strings[1] = build_string_descriptor("Muxella");
    out.strings[2] = build_string_descriptor(out.product);
    out.strings[3] = build_string_descriptor(out.serial);
    return out.validate(err);
}

bool Uac1Descriptors::get_descriptor(uint8_t type, uint8_t index, std::vector<uint8_t>& out) const {
    if (type == kDescDevice && index == 0) {
        out = device;
        return true;
    }
    if (type == kDescConfiguration && index == 0) {
        out = config;
        return true;
    }
    if (type == kDescString && index < strings.size()) {
        out = strings[index];
        return true;
    }
    return false;
}

bool Uac1Descriptors::validate(std::string* err) const {
    if (device.size() != 18 || device[0] != 18 || device[1] != kDescDevice) {
        if (err) *err = "invalid device descriptor";
        return false;
    }
    if (config.size() < 9 || config[1] != kDescConfiguration) {
        if (err) *err = "invalid configuration descriptor";
        return false;
    }
    const uint16_t total = static_cast<uint16_t>(config[2]) | (static_cast<uint16_t>(config[3]) << 8);
    if (total != config.size()) {
        if (err) *err = "configuration wTotalLength mismatch";
        return false;
    }
    for (size_t pos = 0; pos < config.size();) {
        const size_t len = config[pos];
        if (len < 2 || pos + len > config.size()) {
            if (err) *err = "invalid sub-descriptor length";
            return false;
        }
        pos += len;
    }
    return true;
}

Uac1Device::Uac1Device(int number, int latency_ms, int pcm_bits)
    : number_(number),
      bus_id_("1-" + std::to_string(number)),
      buffer_(static_cast<size_t>(kUacSampleRate) * kUacChannels * static_cast<size_t>(pcm_bits / 8) *
              static_cast<size_t>(std::clamp(latency_ms, 10, 2000)) / 1000) {
    std::string ignored;
    Uac1Descriptors::create(number, pcm_bits, descriptors_, &ignored);
}

std::unique_ptr<Uac1Device> Uac1Device::create(int number, int latency_ms, int pcm_bits, std::string* err) {
    Uac1Descriptors test_desc;
    if (!Uac1Descriptors::create(number, pcm_bits, test_desc, err)) {
        return nullptr;
    }
    return std::make_unique<Uac1Device>(number, latency_ms, pcm_bits);
}

int32_t Uac1Device::handle_control(const SetupPacket& setup, const uint8_t* out_data, size_t out_len,
                                   std::vector<uint8_t>& response_data) {
    response_data.clear();
    std::lock_guard<std::mutex> lock(mu_);

    const bool standard = (setup.request_type & 0x60) == 0x00;
    if (standard) {
        switch (setup.request) {
        case kReqGetDescriptor: {
            const uint8_t typ = static_cast<uint8_t>(setup.value >> 8);
            const uint8_t idx = static_cast<uint8_t>(setup.value & 0xFF);
            if (!descriptors_.get_descriptor(typ, idx, response_data)) {
                return -32;
            }
            truncate_vec(response_data, setup.length);
            return 0;
        }
        case kReqSetAddress:
            return 0;
        case kReqSetConfiguration: {
            const uint8_t cfg = static_cast<uint8_t>(setup.value & 0xFF);
            if (cfg > 1) return -32;
            configuration_ = cfg;
            alt_setting_ = {0, 0, 0};
            buffer_.reset();
            return 0;
        }
        case kReqGetConfiguration:
            response_data = {configuration_};
            truncate_vec(response_data, setup.length);
            return 0;
        case kReqSetInterface: {
            const uint8_t iface = static_cast<uint8_t>(setup.index & 0xFF);
            const uint8_t alt = static_cast<uint8_t>(setup.value & 0xFF);
            if (configuration_ != 1 || iface > 2 || alt > 1 || (iface == 0 && alt != 0)) {
                return -32;
            }
            alt_setting_[iface] = alt;
            if (alt == 0 && (iface == 1 || iface == 2)) {
                buffer_.reset();
            }
            return 0;
        }
        case kReqGetInterface: {
            const uint8_t iface = static_cast<uint8_t>(setup.index & 0xFF);
            if (iface > 2) return -32;
            response_data = {alt_setting_[iface]};
            truncate_vec(response_data, setup.length);
            return 0;
        }
        case kReqGetStatus:
            response_data = {0, 0};
            truncate_vec(response_data, setup.length);
            return 0;
        case kReqSynchFrame:
            response_data = {0, 0};
            truncate_vec(response_data, setup.length);
            return 0;
        case kReqClearFeature:
        case kReqSetFeature:
            return 0;
        default:
            return -32;
        }
    }

    const uint8_t recipient = setup.request_type & 0x1F;
    const uint8_t selector = static_cast<uint8_t>(setup.value >> 8);
    const uint8_t endpoint_addr = static_cast<uint8_t>(setup.index & 0xFF);
    if (recipient == 0x02 && selector == 0x01 && (endpoint_addr == 0x01 || endpoint_addr == 0x82)) {
        auto write_rate24 = [&](uint32_t rate) {
            response_data = {
                static_cast<uint8_t>(rate & 0xFF),
                static_cast<uint8_t>((rate >> 8) & 0xFF),
                static_cast<uint8_t>((rate >> 16) & 0xFF)
            };
            truncate_vec(response_data, setup.length);
        };
        switch (setup.request) {
        case kAudioSetCur:
            if (out_data && out_len >= 3) {
                const uint32_t rate = static_cast<uint32_t>(out_data[0]) |
                                      (static_cast<uint32_t>(out_data[1]) << 8) |
                                      (static_cast<uint32_t>(out_data[2]) << 16);
                if (rate != kUacSampleRate) return -32;
                sample_rate_ = rate;
            }
            return 0;
        case kAudioGetCur:
            write_rate24(sample_rate_);
            return 0;
        case kAudioGetMin:
        case kAudioGetMax:
            write_rate24(kUacSampleRate);
            return 0;
        case kAudioGetRes:
            write_rate24(1);
            return 0;
        }
    }

    const uint8_t entity_id = static_cast<uint8_t>(setup.index >> 8);
    const uint8_t iface_num = static_cast<uint8_t>(setup.index & 0xFF);
    if (recipient == 0x01 && iface_num == 0 && (entity_id == 2 || entity_id == 5)) {
        const size_t unit = (entity_id == 5) ? 1 : 0;
        const uint8_t ctrl_sel = static_cast<uint8_t>(setup.value >> 8);
        if (ctrl_sel == 0x01) { // MUTE_CONTROL
            if (setup.request == kAudioSetCur) {
                if (out_data && out_len > 0) mute_[unit] = (out_data[0] != 0);
                return 0;
            }
            if (setup.request == kAudioGetCur) {
                response_data = {static_cast<uint8_t>(mute_[unit] ? 1 : 0)};
                truncate_vec(response_data, setup.length);
                return 0;
            }
        } else if (ctrl_sel == 0x02) { // VOLUME_CONTROL
            auto write_i16 = [&](int16_t v) {
                const uint16_t u = static_cast<uint16_t>(v);
                response_data = {static_cast<uint8_t>(u & 0xFF), static_cast<uint8_t>((u >> 8) & 0xFF)};
                truncate_vec(response_data, setup.length);
            };
            switch (setup.request) {
            case kAudioSetCur:
                if (out_data && out_len >= 2) {
                    const uint16_t u = static_cast<uint16_t>(out_data[0]) | (static_cast<uint16_t>(out_data[1]) << 8);
                    volume_[unit] = static_cast<int16_t>(u);
                }
                return 0;
            case kAudioGetCur:
                write_i16(volume_[unit]);
                return 0;
            case kAudioGetMin:
                write_i16(-60 * 256);
                return 0;
            case kAudioGetMax:
                write_i16(0);
                return 0;
            case kAudioGetRes:
                write_i16(256);
                return 0;
            }
        }
    }

    return -32;
}

bool Uac1Device::playback_active() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return configuration_ == 1 && alt_setting_[1] == 1;
}

bool Uac1Device::capture_active() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return configuration_ == 1 && alt_setting_[2] == 1;
}

size_t Uac1Device::write_playback(uint8_t* data, size_t len) {
    if (!playback_active()) return 0;
    const size_t frame_bytes = static_cast<size_t>(kUacChannels) * static_cast<size_t>(descriptors_.pcm_bytes);
    if (len % frame_bytes != 0) return 0;
    if (!capture_active()) {
        buffer_.reset();
        return len;
    }
    bool mute = false;
    int16_t vol = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        mute = mute_[0];
        vol = volume_[0];
    }
    if (mute || vol != 0) {
        apply_pcm_gain(data, len, mute, vol, descriptors_.pcm_bits, descriptors_.pcm_bytes);
    }
    return buffer_.write(data, len);
}

size_t Uac1Device::read_capture(uint8_t* dst, size_t len) {
    if (!capture_active()) {
        std::memset(dst, 0, len);
        return 0;
    }
    const size_t read = buffer_.read_silence(dst, len);
    bool mute = false;
    int16_t vol = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        mute = mute_[1];
        vol = volume_[1];
    }
    if (mute || vol != 0) {
        apply_pcm_gain(dst, len, mute, vol, descriptors_.pcm_bits, descriptors_.pcm_bytes);
    }
    return read;
}

} // namespace muxella::usbip
