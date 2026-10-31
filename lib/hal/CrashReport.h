#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Pure crash-report decoding (no ESP-IDF dependency, host-tested in test/crash_report).
//
// On ESP32-C3 / IDF 5.5 the RISC-V panic frame's mcause reaches panic_print_backtrace() as
// either a CPU exception code (0..15) or, for SoC-level panics routed through the interrupt
// vector, the interrupt number with the interrupt bit already stripped (24..27). Only the
// abort() family (assert, task WDT, stack canary, heap corruption) supplies a panic message;
// every other panic arrives with an empty one, so the trap registers are the only evidence.
namespace crash_report {

struct Trap {
  uint32_t mepc = 0;
  uint32_t mcause = 0;
  uint32_t mtval = 0;
  uint32_t ra = 0;
  uint32_t sp = 0;
};

inline const char* trapCause(const uint32_t mcause) {
  switch (mcause) {
    case 0: return "Instruction address misaligned";
    case 1: return "Instruction access fault";
    case 2: return "Illegal instruction";
    case 3: return "Breakpoint";
    case 4: return "Load address misaligned";
    case 5: return "Load access fault";
    case 6: return "Store address misaligned";
    case 7: return "Store access fault";
    case 11: return "Environment call from M-mode";
    case 24: return "Interrupt watchdog timeout";
    case 25: return "Cache error";
    case 26: return "Memory protection fault";
    case 27: return "Hardware stack guard fault";
    default: return "Unknown trap";
  }
}

// esp_reset_reason_t values (esp_system.h, IDF 5.5).
inline const char* resetReasonName(const int reason) {
  static const char* const names[] = {"UNKNOWN", "POWERON", "EXT",   "SW",    "PANIC",      "INT_WDT",
                                      "TASK_WDT", "WDT",    "DEEPSLEEP", "BROWNOUT", "SDIO", "USB",
                                      "JTAG",     "EFUSE",  "PWR_GLITCH", "CPU_LOCKUP"};
  return reason >= 0 && reason < static_cast<int>(sizeof(names) / sizeof(names[0])) ? names[reason] : "INVALID";
}

// One-line reason for the on-device crash screen: the abort message when there is one, otherwise
// the decoded trap with its PC, otherwise empty (caller shows its generic "no reason" text).
inline std::string summary(const char* panicMessage, const Trap* trap) {
  if (panicMessage && panicMessage[0] != '\0') return panicMessage;
  if (!trap) return "";
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%s (mcause=%u) at PC 0x%08X", trapCause(trap->mcause),
           static_cast<unsigned>(trap->mcause), static_cast<unsigned>(trap->mepc));
  return buffer;
}

inline std::string trapLines(const Trap& trap) {
  char buffer[160];
  snprintf(buffer, sizeof(buffer), "Trap: %s\nMCAUSE: %u\nMEPC: 0x%08X\nMTVAL: 0x%08X\nRA: 0x%08X\nSP: 0x%08X",
           trapCause(trap.mcause), static_cast<unsigned>(trap.mcause), static_cast<unsigned>(trap.mepc),
           static_cast<unsigned>(trap.mtval), static_cast<unsigned>(trap.ra), static_cast<unsigned>(trap.sp));
  return buffer;
}

}  // namespace crash_report
