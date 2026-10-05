#include "ble/mic_session_owner.h"

#include <stdlib.h>

using namespace remotemapper::ble;

static void require(bool condition) {
    if (!condition) abort();
}

int main() {
    MicSessionOwner owner = {1, 0, 7, true, true, false};
    MicCloseCommand stale_close = {};
    require(prepare_mic_close(owner, 1, &stale_close));

    // A new accepted HOGP down supersedes a queued close before the owner event
    // runs. The stale close must not be returned for transmission.
    const uint32_t new_epoch = candidate_mic_epoch(owner);
    begin_mic_session(owner, new_epoch, 0, true);
    MicCloseCommand command = {};
    require(!take_mic_close(owner, false, &command));
    require(owner.epoch == new_epoch && owner.active && owner.open);
    require(owner.session_id == 0);

    // A matching close is deferred while audio remains active, then accepted
    // once the 60-second guard has stopped the pipeline.
    require(prepare_mic_close(owner, new_epoch, &command));
    require(!take_mic_close(owner, true, &command));
    require(owner.close_pending && owner.open);
    require(take_mic_close(owner, false, &command));
    finish_mic_close(owner, command);
    require(!owner.active && !owner.open);

    // A missing event queue rolls back only the pending close and preserves the
    // owner so ble_remote_task can retry after event initialization.
    begin_mic_session(owner, candidate_mic_epoch(owner), 9, true);
    const uint32_t retry_epoch = owner.epoch;
    require(prepare_mic_close(owner, retry_epoch, &command));
    require(mic_close_enqueue_failed(owner, retry_epoch));
    require(!owner.close_pending && owner.active && owner.open);
    require(prepare_mic_close(owner, retry_epoch, &command));

    disconnect_mic_owner(owner);
    require(!owner.active && !owner.open && !owner.close_pending);

    // HOGP DOWN before AUDIO_START adopts the same token and remote session.
    begin_mic_session(owner, candidate_mic_epoch(owner), 0, true);
    const uint32_t hogp_epoch = owner.epoch;
    require(accept_mic_audio_start(owner, hogp_epoch, true, 84) == hogp_epoch);
    require(owner.session_id == 84);

    // The guard cleared the physical hold while its close remains queued.
    // AUDIO_START for the next press must supersede the stale HOGP token.
    require(prepare_mic_close(owner, hogp_epoch, &command));
    const uint32_t resumed_epoch = accept_mic_audio_start(owner, hogp_epoch, false, 85);
    require(resumed_epoch != hogp_epoch);
    require(!take_mic_close(owner, false, &command));
    require(!prepare_mic_close(owner, hogp_epoch, &command));
    require(owner.active && owner.open && owner.session_id == 85);

    // Even a still-set local press cannot adopt an owner being closed.
    require(prepare_mic_close(owner, resumed_epoch, &command));
    require(accept_mic_audio_start(owner, resumed_epoch, true, 86) != resumed_epoch);
    require(!take_mic_close(owner, false, &command));

    owner.epoch = UINT32_MAX;
    require(candidate_mic_epoch(owner) == 1);
    return 0;
}
