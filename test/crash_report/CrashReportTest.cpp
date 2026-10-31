#include <gtest/gtest.h>

#include "CrashReport.h"

using crash_report::Trap;

// The reported X3 crash: empty panic reason, stack inside the IDLE task. Such a report must
// still carry a decoded cause and PC instead of an empty reason line.
TEST(CrashReport, BlankReasonFallsBackToDecodedTrap) {
  Trap trap;
  trap.mcause = 24;
  trap.mepc = 0x40389514;
  EXPECT_EQ(crash_report::summary("", &trap), "Interrupt watchdog timeout (mcause=24) at PC 0x40389514");
  EXPECT_EQ(crash_report::summary(nullptr, &trap), "Interrupt watchdog timeout (mcause=24) at PC 0x40389514");
}

TEST(CrashReport, AbortMessageWinsAndNoEvidenceStaysEmpty) {
  Trap trap;
  trap.mcause = 7;
  EXPECT_EQ(crash_report::summary("assert failed: heap", &trap), "assert failed: heap");
  EXPECT_EQ(crash_report::summary("", nullptr), "");
}

TEST(CrashReport, DecodesExceptionsAndSocPanicsFromPinnedIdf) {
  EXPECT_STREQ(crash_report::trapCause(2), "Illegal instruction");
  EXPECT_STREQ(crash_report::trapCause(5), "Load access fault");
  EXPECT_STREQ(crash_report::trapCause(7), "Store access fault");
  EXPECT_STREQ(crash_report::trapCause(25), "Cache error");
  EXPECT_STREQ(crash_report::trapCause(26), "Memory protection fault");
  EXPECT_STREQ(crash_report::trapCause(27), "Hardware stack guard fault");
  EXPECT_STREQ(crash_report::trapCause(10), "Unknown trap");
  EXPECT_STREQ(crash_report::trapCause(0x80000018u), "Unknown trap");
}

TEST(CrashReport, ResetReasonNamesMatchIdfEnumAndRejectGarbage) {
  EXPECT_STREQ(crash_report::resetReasonName(4), "PANIC");
  EXPECT_STREQ(crash_report::resetReasonName(5), "INT_WDT");
  EXPECT_STREQ(crash_report::resetReasonName(9), "BROWNOUT");
  EXPECT_STREQ(crash_report::resetReasonName(15), "CPU_LOCKUP");
  EXPECT_STREQ(crash_report::resetReasonName(16), "INVALID");
  EXPECT_STREQ(crash_report::resetReasonName(-1), "INVALID");
}

TEST(CrashReport, TrapLinesCarryAllRegisters) {
  Trap trap{0x4215287E, 5, 0xDEADBEEF, 0x4038870E, 0x3FCA1420};
  const std::string lines = crash_report::trapLines(trap);
  EXPECT_NE(lines.find("Trap: Load access fault"), std::string::npos);
  EXPECT_NE(lines.find("MEPC: 0x4215287E"), std::string::npos);
  EXPECT_NE(lines.find("MTVAL: 0xDEADBEEF"), std::string::npos);
  EXPECT_NE(lines.find("RA: 0x4038870E"), std::string::npos);
  EXPECT_NE(lines.find("SP: 0x3FCA1420"), std::string::npos);
}
