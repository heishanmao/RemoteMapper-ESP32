#include "runtime_timing.h"
#include <cassert>
#include <cstdint>

int main() {
    RuntimeStageTimings timings;
    timings.record(RuntimeStage::Wifi, UINT32_MAX - 9, 40, 100);
    assert(timings.get(RuntimeStage::Wifi).last_us == 50);
    timings.record(RuntimeStage::Web, 10, 50010, 200);
    timings.record(RuntimeStage::Web, 50010, 50020, 201);
    const auto &web = timings.get(RuntimeStage::Web);
    assert(web.calls == 2 && web.last_us == 10 && web.max_us == 50000);
    assert(web.over_budget == 1 && web.max_at_ms == 200);
    assert(timings.get(RuntimeStage::Usb).calls == 0);
    assert(timings.get(RuntimeStage::Wifi).calls == 1);
    timings.record(RuntimeStage::Count, 0, 60000, 300);
    assert(timings.get(RuntimeStage::Usb).calls == 0);
    return 0;
}
