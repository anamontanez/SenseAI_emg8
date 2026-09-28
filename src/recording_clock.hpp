#pragma once
#include <cstdint>

// Bracelet receipt/event clock, microseconds since the current recording epoch.
// This is not the auxiliary sensor's measurement time.
uint64_t recordingElapsedUs();
