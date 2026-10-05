#pragma once

#include <stdint.h>

// Main-loop wall time, including preemption and transport waits. This is not
// CPU utilization. The loop is the only reader/writer of these observations.
enum class RuntimeStage : uint8_t { Usb, Wifi, Web, Cli, Maintenance, Log, Count };

struct RuntimeStageTiming {
    uint32_t calls = 0;
    uint32_t last_us = 0;
    uint32_t max_us = 0;
    uint32_t over_budget = 0;
    uint32_t max_at_ms = 0;
};

class RuntimeStageTimings {
public:
    static constexpr uint32_t budget_us = 50000;
    static constexpr uint8_t count = static_cast<uint8_t>(RuntimeStage::Count);

    void record(RuntimeStage stage, uint32_t started_us, uint32_t ended_us, uint32_t at_ms) {
        const uint8_t index = static_cast<uint8_t>(stage);
        if (index >= count) return;
        RuntimeStageTiming &entry = timings_[index];
        ++entry.calls;
        entry.last_us = ended_us - started_us; // unsigned clock-wrap-safe age
        if (entry.last_us > entry.max_us) {
            entry.max_us = entry.last_us;
            entry.max_at_ms = at_ms;
        }
        if (entry.last_us >= budget_us) ++entry.over_budget;
    }

    const RuntimeStageTiming &get(RuntimeStage stage) const {
        const uint8_t index = static_cast<uint8_t>(stage);
        return timings_[index < count ? index : 0];
    }

private:
    RuntimeStageTiming timings_[count] = {};
};
