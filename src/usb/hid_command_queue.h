#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace remotemapper {

// Single-producer-lock, single-consumer HID command state. Callers protect
// each method with their short command-state critical section; no method waits.
class HidCommandQueue {
public:
    static constexpr uint8_t CAPACITY = 16;
    static constexpr uint8_t NORMAL_LIMIT = CAPACITY - 2; // reserve one release per report

    enum class Kind : uint8_t {
        KeyboardState,
        ConsumerState,
        KeyboardTap,
        ConsumerTap,
        KeyboardRelease,
        ConsumerRelease,
        EmergencyKeyboardRelease,
        EmergencyConsumerRelease
    };

    struct Command {
        Kind kind;
        uint32_t epoch;
        uint32_t channel_epoch;
        uint32_t id;
        uint8_t modifier;
        uint8_t keycode;
        uint16_t consumer;
    };

    HidCommandQueue() : m_head(0), m_count(0), m_epoch(1),
            m_keyboard_epoch(1), m_consumer_epoch(1), m_next_id(1),
            m_emergency_keyboard(false), m_emergency_consumer(false),
            m_recovering(false), m_detaching(false), m_sending(false), m_sending_channel(0) {}

    bool enqueue_keyboard(uint8_t modifier, uint8_t keycode, bool tap) {
        if (m_recovering || m_count >= NORMAL_LIMIT) {
            force_release_all();
            return false;
        }
        Command command = make(tap ? Kind::KeyboardTap : Kind::KeyboardState,
                0, modifier, keycode, m_keyboard_epoch);
        push(command);
        return true;
    }

    bool enqueue_consumer(uint16_t usage, bool tap) {
        if (m_recovering || m_count >= NORMAL_LIMIT) {
            force_release_all();
            return false;
        }
        Command command = make(tap ? Kind::ConsumerTap : Kind::ConsumerState,
                usage, 0, 0, m_consumer_epoch);
        push(command);
        return true;
    }

    // Ordinary releases are FIFO commands, preserving a short press followed
    // immediately by release. Overflow falls back to an epoch-cancelling zero.
    bool enqueue_keyboard_release() {
        if (m_recovering || m_count >= CAPACITY) {
            force_release_all();
            return true;
        }
        push(make(Kind::KeyboardRelease, 0, 0, 0, m_keyboard_epoch));
        return true;
    }

    bool enqueue_consumer_release() {
        if (m_recovering || m_count >= CAPACITY) {
            force_release_all();
            return true;
        }
        push(make(Kind::ConsumerRelease, 0, 0, 0, m_consumer_epoch));
        return true;
    }

    void force_release_all() {
        m_epoch = next(m_epoch);
        m_keyboard_epoch = next(m_keyboard_epoch);
        m_consumer_epoch = next(m_consumer_epoch);
        m_head = 0;
        m_count = 0;
        m_emergency_keyboard = true;
        m_emergency_consumer = true;
    }

    void begin_recovery() {
        m_recovering = true;
        m_detaching = false;
        force_release_all();
    }

    void end_recovery() { m_recovering = false; m_detaching = false; }
    void pause_for_detach() { m_detaching = true; }
    bool recovering() const { return m_recovering; }

    bool peek(Command &out) {
        if (m_emergency_keyboard) {
            out = make(Kind::EmergencyKeyboardRelease, 0, 0, 0, m_keyboard_epoch);
            return true;
        }
        if (m_emergency_consumer) {
            out = make(Kind::EmergencyConsumerRelease, 0, 0, 0, m_consumer_epoch);
            return true;
        }
        discard_stale_front();
        if (!m_count) return false;
        out = m_commands[m_head];
        return true;
    }

    bool is_current(const Command &command) const {
        if (command.epoch != m_epoch) return false;
        if (is_keyboard(command.kind)) return command.channel_epoch == m_keyboard_epoch;
        return command.channel_epoch == m_consumer_epoch;
    }

    bool begin_send(const Command &command) {
        if (m_sending || !is_current(command)) return false;
        if (m_detaching || (m_recovering && !is_emergency(command.kind))) return false;
        if (is_emergency(command.kind) && !emergency_is_pending(command)) return false;
        if (!is_emergency(command.kind) && (!m_count || m_commands[m_head].id != command.id)) return false;
        m_sending = true;
        m_sending_channel = is_keyboard(command.kind) ? 1 : 2;
        return true;
    }

    void end_send() {
        m_sending = false;
        m_sending_channel = 0;
    }

    bool complete(const Command &command) {
        if (!is_current(command)) return false;
        if (command.kind == Kind::EmergencyKeyboardRelease) {
            if (!m_emergency_keyboard) return false;
            m_emergency_keyboard = false;
            return true;
        }
        if (command.kind == Kind::EmergencyConsumerRelease) {
            if (!m_emergency_consumer) return false;
            m_emergency_consumer = false;
            return true;
        }
        if (!m_count || m_commands[m_head].id != command.id) return false;
        m_head = (uint8_t)((m_head + 1) % CAPACITY);
        --m_count;
        discard_stale_front();
        return true;
    }

    bool has_pending(uint8_t channel) const {
        if (channel == 1 && (m_emergency_keyboard || (m_sending && m_sending_channel == 1))) return true;
        if (channel == 2 && (m_emergency_consumer || (m_sending && m_sending_channel == 2))) return true;
        for (uint8_t i = 0; i < m_count; ++i) {
            const Command &command = m_commands[(m_head + i) % CAPACITY];
            if (is_current(command) && (channel == 1 ? is_keyboard(command.kind) : !is_keyboard(command.kind)))
                return true;
        }
        return false;
    }

    bool has_pending() const { return has_pending(1) || has_pending(2); }
    bool sender_busy() const { return m_sending; }
    bool emergency_pending() const { return m_emergency_keyboard || m_emergency_consumer; }
    uint8_t size() const { return m_count; }

private:
    static uint32_t next(uint32_t value) { ++value; return value ? value : 1; }
    static bool is_keyboard(Kind kind) {
        return kind == Kind::KeyboardState || kind == Kind::KeyboardTap ||
                kind == Kind::KeyboardRelease || kind == Kind::EmergencyKeyboardRelease;
    }
    static bool is_emergency(Kind kind) {
        return kind == Kind::EmergencyKeyboardRelease || kind == Kind::EmergencyConsumerRelease;
    }
    bool emergency_is_pending(const Command &command) const {
        return command.kind == Kind::EmergencyKeyboardRelease ? m_emergency_keyboard : m_emergency_consumer;
    }
    Command make(Kind kind, uint16_t consumer, uint8_t modifier, uint8_t keycode, uint32_t channel_epoch) {
        Command command = {};
        command.kind = kind;
        command.epoch = m_epoch;
        command.channel_epoch = channel_epoch;
        command.id = m_next_id;
        m_next_id = next(m_next_id);
        command.modifier = modifier;
        command.keycode = keycode;
        command.consumer = consumer;
        return command;
    }
    void push(const Command &command) {
        m_commands[(m_head + m_count) % CAPACITY] = command;
        ++m_count;
    }
    void discard_stale_front() {
        while (m_count && !is_current(m_commands[m_head])) {
            m_head = (uint8_t)((m_head + 1) % CAPACITY);
            --m_count;
        }
    }

    Command m_commands[CAPACITY];
    uint8_t m_head;
    uint8_t m_count;
    uint32_t m_epoch;
    uint32_t m_keyboard_epoch;
    uint32_t m_consumer_epoch;
    uint32_t m_next_id;
    bool m_emergency_keyboard;
    bool m_emergency_consumer;
    bool m_recovering;
    bool m_detaching;
    bool m_sending;
    uint8_t m_sending_channel;
};

} // namespace remotemapper
