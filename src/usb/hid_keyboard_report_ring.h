#pragma once

#include <stddef.h>
#include <stdint.h>

namespace remotemapper {
namespace hid_diag {

class KeyboardReportRing {
public:
    static constexpr size_t CAPACITY = 32;
    static constexpr size_t REPORT_BYTES = 8;

    enum class Source : uint8_t { Queue = 0, Stress = 1 };

    struct Entry {
        uint32_t first_ms;
        uint32_t last_ms;
        uint32_t command_id;
        uint32_t epoch;
        uint32_t count;
        uint8_t report[REPORT_BYTES];
        Source source;
        bool completed;
    };

    KeyboardReportRing() : m_head(0), m_size(0) {}

    void record(uint32_t ms, Source source, uint32_t command_id, uint32_t epoch,
            const uint8_t report[REPORT_BYTES], bool completed) {
        if (!report) return;
        if (m_size) {
            Entry &last = m_entries[(m_head + m_size - 1) % CAPACITY];
            if (same(last, source, command_id, epoch, report, completed)) {
                last.last_ms = ms;
                if (last.count != UINT32_MAX) ++last.count;
                return;
            }
        }

        size_t index;
        if (m_size < CAPACITY) {
            index = (m_head + m_size) % CAPACITY;
            ++m_size;
        } else {
            index = m_head;
            m_head = (m_head + 1) % CAPACITY;
        }
        Entry &entry = m_entries[index];
        entry.first_ms = ms;
        entry.last_ms = ms;
        entry.command_id = command_id;
        entry.epoch = epoch;
        entry.count = 1;
        for (size_t i = 0; i < REPORT_BYTES; ++i) entry.report[i] = report[i];
        entry.source = source;
        entry.completed = completed;
    }

    size_t size() const { return m_size; }
    const Entry &at(size_t chronological_index) const {
        return m_entries[(m_head + chronological_index) % CAPACITY];
    }

private:
    static bool same(const Entry &entry, Source source, uint32_t command_id,
            uint32_t epoch, const uint8_t report[REPORT_BYTES], bool completed) {
        if (entry.source != source || entry.command_id != command_id ||
                entry.epoch != epoch || entry.completed != completed) return false;
        for (size_t i = 0; i < REPORT_BYTES; ++i)
            if (entry.report[i] != report[i]) return false;
        return true;
    }

    Entry m_entries[CAPACITY] = {};
    size_t m_head;
    size_t m_size;
};

} // namespace hid_diag
} // namespace remotemapper
