#pragma once

// Host-supplied codes, never filesystem paths or personal/demographic data.
// Fixed capacity and a restricted alphabet keep both UART and JSON unambiguous.
struct RecordingIdentity {
    char subject[33]{};
    char session[65]{};
};

constexpr bool identityCharacter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

constexpr bool parseRecordingIdentity(const char* text, int length,
                                      RecordingIdentity& result) {
    RecordingIdentity candidate{};
    int split = -1;
    for (int i = 0; i < length; ++i) {
        if (text[i] == ',' && split < 0) split = i;
        else if (!identityCharacter(text[i])) return false;
    }
    if (split < 1 || split > 32 || length - split - 1 < 1 ||
        length - split - 1 > 64) return false;
    for (int i = 0; i < split; ++i) candidate.subject[i] = text[i];
    for (int i = split + 1; i < length; ++i) candidate.session[i - split - 1] = text[i];
    result = candidate;
    return true;
}

// Opt-in for a tagged study. Every new recording needs an explicit tag; a
// successful start consumes it. Rejected commands disarm any previous tag.
// Aborted starts retain the selection for retry. J- explicitly exits this mode.
struct RecordingIdentitySelection {
    RecordingIdentity value{};
    bool required = false;
    bool armed = false;

    constexpr bool set(const char* text, int length) {
        required = true;
        armed = false;
        value = {};
        armed = parseRecordingIdentity(text, length, value);
        return armed;
    }
    constexpr bool canStart() const { return !required || armed; }
    constexpr void consume() { value = {}; armed = false; }
    constexpr void clear() { consume(); required = false; }
    constexpr const char* state() const {
        return armed ? "ARMED" : required ? "REQUIRED" : "OFF";
    }
};
