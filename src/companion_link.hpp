#pragma once
#include <cstdint>

enum class SessionPhase : uint8_t { Grasp = 0x04, Rest = 0x05, Demo = 0x06 };
const char* phaseName(SessionPhase phase);

// Called from the core-0 control task only. All UART1 writes belong to linkTask.
void companionInit();
bool companionStart(int64_t epochUs, SessionPhase phase);
bool companionStop(bool notify);
bool companionPhase(SessionPhase phase);
void companionPrintStats();
