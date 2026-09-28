#pragma once

// Core-0 companion task: bounded UART1 draining, never writes to host UART0.
void companionReceive();
// Core-0 UART preview task: one pending batch/second while live, also in U0.
// Each unchanged auxiliary line is paired with a #AUXTS receipt-time sideband.
// Passing false drains/discards pending lines (stopped, file download).
void companionForward(bool live);
void companionRxPrintStats();
