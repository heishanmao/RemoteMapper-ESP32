#pragma once

#include <stdint.h>

namespace remotemapper {
namespace ble {

constexpr uint8_t kVoiceCodec16k = 0x02;

inline bool voice_link_ready(bool handshake_complete, bool command_available,
                             bool caps_received, uint8_t codec_mask) {
    return handshake_complete && command_available && caps_received &&
            (codec_mask & kVoiceCodec16k) != 0;
}

inline bool voice_action_needs_pipeline_start(bool action_accepted,
                                               bool pipeline_active) {
    return action_accepted && !pipeline_active;
}

// Gates only ACTION_VOICE_HOLD dispatch. A press rejected before readiness is
// deliberately not replayed when CAPS arrives; the user must press again.
class VoiceActionGate {
public:
    void set_ready(bool ready) { ready_ = ready; }

    // Returns true when the caller should execute the hold action now.
    bool press() {
        if (!ready_) return false;
        active_ = true;
        return true;
    }

    // Returns true when an already-emitted hold needs its matching release.
    bool release() {
        const bool release_active = active_;
        active_ = false;
        return release_active;
    }

    // A failed remote MIC_OPEN or a disconnect invalidates the local hold.
    bool invalidate() {
        return release();
    }

    void reset_connection() {
        ready_ = false;
        active_ = false;
    }

    bool active() const { return active_; }
    bool ready() const { return ready_; }

private:
    bool ready_ = false;
    bool active_ = false;
};

} // namespace ble
} // namespace remotemapper
