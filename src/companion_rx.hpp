#pragma once

// Core-0 companion task: bounded UART1 draining, never writes to host UART0.
void companionReceive();
// Core-0 UART preview task: one pending batch/second while live and U1.
// Passing false drains/discards pending lines (quiet, stopped, file download).
void companionForward(bool live);
void companionRxPrintStats();
