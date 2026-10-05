#pragma once

#include <stdint.h>

namespace remotemapper {
namespace ble {

struct MicSessionOwner {
    uint32_t epoch;
    uint32_t pending_close_epoch;
    uint8_t session_id;
    bool active;
    bool open;
    bool close_pending;
};

struct MicCloseCommand {
    uint32_t epoch;
    uint8_t session_id;
};

inline uint32_t next_mic_epoch(uint32_t epoch) {
    ++epoch;
    return epoch ? epoch : 1u;
}

inline uint32_t candidate_mic_epoch(const MicSessionOwner &owner) {
    return next_mic_epoch(owner.epoch);
}

// Commit only after the engine accepts the corresponding physical press.
// Call while holding the engine transaction boundary and mic-state mux.
inline void begin_mic_session(MicSessionOwner &owner, uint32_t epoch,
                              uint8_t session_id, bool mic_open) {
    owner.epoch = epoch ? epoch : 1u;
    owner.pending_close_epoch = 0;
    owner.session_id = session_id;
    owner.active = true;
    owner.open = mic_open;
    owner.close_pending = false;
}

// AUDIO_START may precede or follow the HOGP DOWN for the same physical hold.
// A hold cleared by the guard is no longer that owner, even if the host-only
// HOGP token has not yet seen a release report.
inline uint32_t accept_mic_audio_start(MicSessionOwner &owner,
        uint32_t hogp_epoch, bool already_pressed, uint8_t session_id) {
    if (!already_pressed || !hogp_epoch || owner.epoch != hogp_epoch ||
            !owner.active || owner.close_pending) {
        const uint32_t epoch = candidate_mic_epoch(owner);
        begin_mic_session(owner, epoch, session_id, true);
        return epoch;
    }
    owner.session_id = session_id;
    owner.open = true;
    return owner.epoch;
}

inline bool prepare_mic_close(MicSessionOwner &owner, uint32_t expected_epoch,
                              MicCloseCommand *command) {
    if (!owner.active || !owner.open || owner.epoch != expected_epoch) return false;
    if (owner.close_pending && owner.pending_close_epoch != expected_epoch) return false;
    owner.close_pending = true;
    owner.pending_close_epoch = expected_epoch;
    if (command) {
        command->epoch = expected_epoch;
        command->session_id = owner.session_id;
    }
    return true;
}

// A failed bridge to the NimBLE event queue leaves the owner open and eligible
// for retry. The caller must restore the request flag after this returns true.
inline bool mic_close_enqueue_failed(MicSessionOwner &owner, uint32_t expected_epoch) {
    if (!owner.close_pending || owner.pending_close_epoch != expected_epoch) return false;
    owner.close_pending = false;
    owner.pending_close_epoch = 0;
    return true;
}

inline bool take_mic_close(MicSessionOwner &owner, bool pipeline_active,
                           MicCloseCommand *command) {
    if (!owner.close_pending) return false;
    if (owner.pending_close_epoch != owner.epoch || !owner.active || !owner.open) {
        owner.close_pending = false;
        owner.pending_close_epoch = 0;
        return false;
    }
    if (pipeline_active) return false;
    if (command) {
        command->epoch = owner.epoch;
        command->session_id = owner.session_id;
    }
    owner.close_pending = false;
    owner.pending_close_epoch = 0;
    return true;
}

inline void finish_mic_close(MicSessionOwner &owner, const MicCloseCommand &command) {
    if (owner.epoch != command.epoch) return;
    owner.active = false;
    owner.open = false;
    owner.session_id = 0;
    if (owner.close_pending && owner.pending_close_epoch == command.epoch) {
        owner.close_pending = false;
        owner.pending_close_epoch = 0;
    }
}

inline void disconnect_mic_owner(MicSessionOwner &owner) {
    owner.epoch = next_mic_epoch(owner.epoch);
    owner.pending_close_epoch = 0;
    owner.session_id = 0;
    owner.active = false;
    owner.open = false;
    owner.close_pending = false;
}

} // namespace ble
} // namespace remotemapper
