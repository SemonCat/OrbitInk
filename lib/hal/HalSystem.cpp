#include "HalSystem.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "Arduino.h"
#include "CrashReport.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include "esp_core_dump.h"
#define ORBITINK_COREDUMP_EXPORT 1
#endif

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

// Trap registers of the panicking context. Non-abort panics (CPU exceptions, interrupt watchdog,
// cache/memory-protection/stack-guard faults) carry no panic message, so without these a report
// shows only raw stack words. Valid only while panicTrapMagic matches; cleared with the rest.
RTC_NOINIT_ATTR uint32_t panicTrapMagic;
RTC_NOINIT_ATTR crash_report::Trap panicTrap;
constexpr uint32_t PANIC_TRAP_MAGIC = 0x50415254;  // 'TRAP'

// Plain DRAM (.bss), zeroed every boot: true only when this boot's panic went through
// panic_abort(). Lets the backtrace hook drop a message left behind in RTC memory by an earlier,
// not-yet-reported panic instead of pairing it with this panic's registers and stack.
static bool panicAbortRecorded = false;

// Identity of the app binary that most recently started executing setup() -- see
// HalSystem::recordBootIdentity(). Distinct from panicMessage/panicStack above: those get
// explicitly cleared by clearPanic(), but this is intentionally never cleared -- it's
// unconditionally overwritten every boot, and a stale (mismatched) value left behind by a
// previous binary is the signal, not a hazard to guard against.
RTC_NOINIT_ATTR uint32_t appIdentityMagic;
RTC_NOINIT_ATTR uint8_t appIdentityHash[32];
constexpr uint32_t APP_IDENTITY_MAGIC = 0x44495041;  // on disk (little-endian): 'A', 'P', 'I', 'D'

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  panicAbortRecorded = true;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  if (!panicAbortRecorded) {
    panicMessage[0] = '\0';
  }
  const RvExcFrame* exc = static_cast<const RvExcFrame*>(frame);
  panicTrap.mepc = exc->mepc;
  panicTrap.mcause = exc->mcause;
  panicTrap.mtval = exc->mtval;
  panicTrap.ra = exc->ra;
  panicTrap.sp = exc->sp;
  panicTrapMagic = PANIC_TRAP_MAGIC;

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

namespace {
const crash_report::Trap* recordedTrap() { return panicTrapMagic == PANIC_TRAP_MAGIC ? &panicTrap : nullptr; }

#if ORBITINK_COREDUMP_EXPORT
// The flash core dump (64 KiB partition) holds the faulting task, PC and backtrace that the RTC
// report cannot. It persists across reboots and OTA updates until erased, so export it on every
// boot -- not only panic boots -- and erase the flash copy only after the SD copy is complete.
void exportCoreDump() {
  if (esp_core_dump_image_check() != ESP_OK) return;
  size_t address = 0;
  size_t size = 0;
  if (esp_core_dump_image_get(&address, &size) != ESP_OK || size == 0) return;

  std::unique_ptr<esp_core_dump_summary_t> summary(new (std::nothrow) esp_core_dump_summary_t());
  if (summary && esp_core_dump_get_summary(summary.get()) == ESP_OK) {
    char text[384];
    const int shaLen = static_cast<int>(strnlen(reinterpret_cast<const char*>(summary->app_elf_sha256),
                                                sizeof(summary->app_elf_sha256)));
    snprintf(text, sizeof(text),
             "Core dump task: %.16s\nPC: 0x%08X\nApp ELF SHA256 prefix: %.*s\nMCAUSE: %u (%s)\nMTVAL: 0x%08X\n"
             "RA: 0x%08X\nSP: 0x%08X\nCore dump: /crash_core.elf (%u bytes)\n",
             summary->exc_task, static_cast<unsigned>(summary->exc_pc), shaLen,
             reinterpret_cast<const char*>(summary->app_elf_sha256), static_cast<unsigned>(summary->ex_info.mcause),
             crash_report::trapCause(summary->ex_info.mcause), static_cast<unsigned>(summary->ex_info.mtval),
             static_cast<unsigned>(summary->ex_info.ra), static_cast<unsigned>(summary->ex_info.sp),
             static_cast<unsigned>(size));
    Storage.writeFile("/crash_core.txt", String(text));
  }
  summary.reset();

  constexpr size_t CHUNK = 4096;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[CHUNK]);
  if (!buffer) return;
  auto file = Storage.open("/crash_core.elf", O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("SYS", "Failed to open crash_core.elf for writing");
    return;
  }
  size_t copied = 0;
  while (copied < size) {
    const size_t n = std::min(CHUNK, size - copied);
    if (esp_flash_read(nullptr, buffer.get(), address + copied, n) != ESP_OK || file.write(buffer.get(), n) != n) {
      break;
    }
    copied += n;
  }
  file.close();
  if (copied != size) {
    LOG_ERR("SYS", "Core dump export incomplete (%u/%u bytes); flash copy kept", static_cast<unsigned>(copied),
            static_cast<unsigned>(size));
    return;
  }
  esp_core_dump_image_erase();
  LOG_INF("SYS", "Exported core dump to SD card (%u bytes)", static_cast<unsigned>(size));
}
#endif

// Set once by recordBootIdentity() at the very start of setup(), before anything that could hang
// or panic -- see isRebootFromPanic(), which reads it for the rest of this boot.
bool bootIdentityMatchesLastBoot = false;
}  // namespace

void recordBootIdentity() {
  const esp_app_desc_t* desc = esp_ota_get_app_description();
  const bool hadValidMarker = appIdentityMagic == APP_IDENTITY_MAGIC;
  bootIdentityMatchesLastBoot =
      hadValidMarker && memcmp(appIdentityHash, desc->app_elf_sha256, sizeof(appIdentityHash)) == 0;
  memcpy(appIdentityHash, desc->app_elf_sha256, sizeof(appIdentityHash));
  appIdentityMagic = APP_IDENTITY_MAGIC;
}

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
#if ORBITINK_COREDUMP_EXPORT
  exportCoreDump();
#endif
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicMessage[0] = '\0';
  panicTrapMagic = 0;
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return crash_report::summary(panicMessage, recordedTrap());
  } else {
    std::string info;

    info += "OrbitInk version: " CROSSPOINT_VERSION;
#if defined(CROSSFADE_OTA_CHANNEL) && defined(CROSSFADE_OTA_BUILD)
    info += "\nChannel: " CROSSFADE_OTA_CHANNEL "\nBuild: " + std::to_string(CROSSFADE_OTA_BUILD);
#endif
    {
      // Identifies the exact ELF needed to symbolize the addresses below.
      const esp_app_desc_t* desc = esp_ota_get_app_description();
      char sha[17];
      for (size_t i = 0; i < 8; i++) snprintf(sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
      info += "\nApp: " + std::string(desc->version) + "\nApp ELF SHA256 prefix: " + sha;
    }
    info += "\nReset reason: " + std::string(crash_report::resetReasonName(esp_reset_reason()));
    info += "\n\nPanic reason: " + std::string(panicMessage);
    if (const auto* trap = recordedTrap()) {
      info += "\n" + crash_report::trapLines(*trap);
    }
    info += "\n\nLast logs (may include the previous boot; the reboot starts at the low timestamps):\n" +
            getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  const bool panicOrWatchdogReason = resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP ||
                                      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT ||
                                      resetReason == ESP_RST_WDT;
  if (!panicOrWatchdogReason) return false;
  // On native-USB ESP32-C3 boards (no host-accessible EN line), esptool reboots into the
  // newly-flashed app via the same RTC-watchdog reset path a genuine hang would trip -- so a
  // watchdog-family reset reason alone can't tell "just flashed" apart from "actually hung".
  // recordBootIdentity() (called first thing in setup(), before anything that could hang) only
  // sees a match here if the currently-running binary was already executing before this reset
  // happened, which a flash-triggered reboot -- reset before this binary's setup() ever ran --
  // can't produce.
  return bootIdentityMatchesLastBoot;
}

}  // namespace HalSystem
