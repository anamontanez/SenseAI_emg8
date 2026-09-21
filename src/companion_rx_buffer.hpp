#pragma once
#include <atomic>
#include <cstdint>

// Single producer (companion task), single consumer (UART preview task).
// Publish only complete lines. A slow host never blocks the companion or ADCs.
class CompanionRxBuffer {
public:
    static constexpr uint32_t capacity = 4096;
    static constexpr uint32_t maxLine = 3078; // Ana's 3072-byte payload + imp:[]
    enum class Result { None, Accepted, Rejected, Full };
    struct View { const char* first; uint32_t firstSize; const char* second;
                  uint32_t secondSize; uint32_t end; };

    Result feed(uint8_t byte) {
        if (byte == '\n') {
            if (dropping_) { resetPartial(); return Result::None; }
            // Accept CRLF as well as LF, forwarding canonical LF.
            if (used_ && at(used_ - 1) == '\r') --used_;
            if (!valid()) { resetPartial(); return Result::Rejected; }
            if (!room()) { resetPartial(); return Result::Full; }
            bytes_[(begin_ + used_) % capacity] = '\n';
            begin_ = (begin_ + used_ + 1) % capacity;
            published_.store(begin_, std::memory_order_release);
            used_ = 0;
            return Result::Accepted;
        }
        if (dropping_) return Result::None;
        if ((byte < 32 && byte != '\r') || byte > 126 || used_ >= maxLine + 1) {
            losePartial(); return Result::Rejected;
        }
        if (!room()) { losePartial(); return Result::Full; }
        bytes_[(begin_ + used_++) % capacity] = static_cast<char>(byte);
        return Result::None;
    }

    // On wire errors, discard through the next newline to regain framing.
    void losePartial() { used_ = 0; dropping_ = true; }
    uint32_t snapshot() const {
        return published_.load(std::memory_order_acquire);
    }
    // Consumer holds each view until consume(); producer cannot overwrite it.
    bool peek(uint32_t limit, View& view) const {
        uint32_t start = consumed_.load(std::memory_order_relaxed), end = start;
        if (start == limit) return false;
        while (end != limit && bytes_[end] != '\n') end = (end + 1) % capacity;
        if (end == limit) return false;
        uint32_t size = (end + capacity - start) % capacity;
        uint32_t first = size < capacity - start ? size : capacity - start;
        view = {bytes_ + start, first, bytes_, size - first, (end + 1) % capacity};
        return true;
    }
    void consume(const View& view) {
        consumed_.store(view.end, std::memory_order_release);
    }

private:
    char bytes_[capacity]{};
    std::atomic<uint32_t> published_{0}, consumed_{0};
    uint32_t begin_ = 0, used_ = 0;
    bool dropping_ = false;
    char at(uint32_t index) const { return bytes_[(begin_ + index) % capacity]; }
    bool room() const {
        return (begin_ + used_ + 1) % capacity != consumed_.load(std::memory_order_acquire);
    }
    void resetPartial() { used_ = 0; dropping_ = false; }
    bool matches(uint32_t pos, const char* text) const {
        for (uint32_t i = 0; text[i]; ++i)
            if (pos + i >= used_ || at(pos + i) != text[i]) return false;
        return true;
    }
    bool contains(const char* text) const {
        for (uint32_t i = 0; i < used_; ++i) if (matches(i, text)) return true;
        return false;
    }
    bool valid() const {
        if (!used_ || used_ > maxLine) return false;
        for (uint32_t i = 0; i < used_; ++i) if (at(i) == '\r') return false;
        return (used_ > 6 && matches(0, "imp:[") && at(used_ - 1) == ']') ||
               (matches(0, "p1:") && contains(",p2:") && contains(",temp:"));
    }
};
