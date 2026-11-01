// Owned simulator adapter: the HalPowerManager methods CrossFade adds on top of the pinned
// crosspoint-simulator HAL. See sim/CrossFadeSimHal/HalPowerManager.h for why this lives here
// rather than in the dependency, and sim/README.md for the boundary rules.

#include <HalPowerManager.h>
#include <SimulatorLifecycle.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
// Opt-in accelerated timer sleep (all host-only, see sim/README.md):
//   CROSSFADE_SIM_TIMER_DIVISOR        wait delay/divisor, then relaunch as a timer wake
//   CROSSFADE_SIM_TIMER_MAX_WAKES      relaunches left; at 0, or with nothing armed, exit(0)
//   CROSSFADE_SIM_TIMER_HEAP_SCHEDULE  per relaunch "free:largest" or "-", consumed in order
//   CROSSFADE_SIM_TIMER_TRACE          absolute path; one line per sleep with the armed delay
// Internal, carried across every relaunch (timer or button) the way RTC memory survives deep sleep:
//   CROSSFADE_SIM_TIMER_WAKE           read once by crossfadeWakeupReason() (HalGPIOCompat.cpp)
//   CROSSFADE_SIM_RTC_TIMER_DELAY      the RTC copy of the armed delay
//   CROSSFADE_SIM_RTC_SLEEP_CYCLES     the RTC sleep counter
constexpr char DIVISOR_ENV[] = "CROSSFADE_SIM_TIMER_DIVISOR";
constexpr char MAX_WAKES_ENV[] = "CROSSFADE_SIM_TIMER_MAX_WAKES";
constexpr char HEAP_SCHEDULE_ENV[] = "CROSSFADE_SIM_TIMER_HEAP_SCHEDULE";
constexpr char TRACE_ENV[] = "CROSSFADE_SIM_TIMER_TRACE";
constexpr char TIMER_WAKE_ENV[] = "CROSSFADE_SIM_TIMER_WAKE";
constexpr char RTC_DELAY_ENV[] = "CROSSFADE_SIM_RTC_TIMER_DELAY";
constexpr char RTC_CYCLES_ENV[] = "CROSSFADE_SIM_RTC_SLEEP_CYCLES";
constexpr char FREE_HEAP_ENV[] = "CROSSPOINT_SIM_FREE_HEAP";
constexpr char MAX_ALLOC_ENV[] = "CROSSPOINT_SIM_MAX_ALLOC_HEAP";

// rebootAsPowerWake() promotes these onto whatever process it launches. They belong to the user's
// eventual power-button wake, never to an unattended timer wake, so a timer relaunch parks them.
constexpr const char* AFTER_WAKE_ENVS[][2] = {
    {"CROSSPOINT_SIM_INPUT_SCRIPT_AFTER_WAKE", "CROSSFADE_SIM_PARKED_INPUT_SCRIPT_AFTER_WAKE"},
    {"CROSSPOINT_SIM_SCREENSHOTS_AFTER_WAKE", "CROSSFADE_SIM_PARKED_SCREENSHOTS_AFTER_WAKE"},
};

bool readUnsigned(const char* name, uint64_t& value) {
  const char* text = std::getenv(name);
  if (!text || !*text) return false;
  char* end = nullptr;
  value = std::strtoull(text, &end, 10);
  return *end == '\0';
}

void moveEnv(const char* from, const char* to) {
  if (const char* value = std::getenv(from)) {
    const std::string copy(value);
    setenv(to, copy.c_str(), 1);
    unsetenv(from);
  }
}

void trace(const char* action, uint64_t delayMicros) {
  const char* path = std::getenv(TRACE_ENV);
  if (!path || path[0] != '/') return;
  if (FILE* file = std::fopen(path, "a")) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::fprintf(file, "t=%lld.%03lld pid=%d armed_us=%" PRIu64 " action=%s\n", static_cast<long long>(ms / 1000),
                 static_cast<long long>(ms % 1000), static_cast<int>(getpid()), delayMicros, action);
    std::fclose(file);
  }
}

// The schedule's first entry becomes the relaunched process's static heap figures (read on every
// call by the simulator's ESP mock). These are chosen numbers, not allocator tracking.
void applyNextHeapEntry() {
  const char* schedule = std::getenv(HEAP_SCHEDULE_ENV);
  if (!schedule) return;
  std::string rest(schedule);
  const size_t comma = rest.find(',');
  const std::string entry = rest.substr(0, comma);
  rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
  const size_t colon = entry.find(':');
  if (entry == "-" || entry.empty() || colon == std::string::npos) {
    unsetenv(FREE_HEAP_ENV);
    unsetenv(MAX_ALLOC_ENV);
  } else {
    setenv(FREE_HEAP_ENV, entry.substr(0, colon).c_str(), 1);
    setenv(MAX_ALLOC_ENV, entry.substr(colon + 1).c_str(), 1);
  }
  setenv(HEAP_SCHEDULE_ENV, rest.c_str(), 1);
}

// Button wake: the user's interactive session gets back what timer relaunches parked, and no
// heap figure scheduled for an unattended wake.
void prepareButtonWake() {
  for (const auto& names : AFTER_WAKE_ENVS) moveEnv(names[1], names[0]);
  if (std::getenv(HEAP_SCHEDULE_ENV)) {
    unsetenv(FREE_HEAP_ENV);
    unsetenv(MAX_ALLOC_ENV);
  }
}
}  // namespace

uint64_t HalPowerManager::retainedTimerDelayMicros() {
  uint64_t value = 0;
  return readUnsigned(RTC_DELAY_ENV, value) ? value : 0;
}

uint32_t HalPowerManager::retainedSleepCycles() {
  uint64_t value = 0;
  return readUnsigned(RTC_CYCLES_ENV, value) ? static_cast<uint32_t>(value) : 0;
}

// Device semantics (lib/hal/HalPowerManager.cpp): deep sleep with an RTC timer wake scheduled
// `sleepDelayMicros` from now, on top of the button wakeups the pinned single-argument overload
// already covers. Without the divisor the delay is unused and this blocks for a button wake.
void HalPowerManager::startDeepSleep(HalGPIO& gpio, uint64_t sleepDelayMicros) const {
  extern void crossfadeSimCaptureSleep();
  crossfadeSimCaptureSleep();
  // Device: the RTC delay is cleared, then set only when a timer is armed; every sleep counts.
  if (sleepDelayMicros != 0) {
    setenv(RTC_DELAY_ENV, std::to_string(sleepDelayMicros).c_str(), 1);
  } else {
    unsetenv(RTC_DELAY_ENV);
  }
  setenv(RTC_CYCLES_ENV, std::to_string(retainedSleepCycles() + 1).c_str(), 1);

  uint64_t divisor = 0;
  if (!readUnsigned(DIVISOR_ENV, divisor) || divisor == 0) {
    trace("button-only", sleepDelayMicros);
    prepareButtonWake();
    startDeepSleep(gpio);
    return;
  }
  uint64_t wakesLeft = 0;
  const bool bounded = readUnsigned(MAX_WAKES_ENV, wakesLeft);
  if (sleepDelayMicros == 0 || (bounded && wakesLeft == 0)) {
    trace(sleepDelayMicros == 0 ? "unarmed" : "budget-spent", sleepDelayMicros);
    if (bounded) _exit(0);  // a bounded run is over: nothing else will ever wake this process
    prepareButtonWake();
    startDeepSleep(gpio);
    return;
  }

  trace("timer", sleepDelayMicros);
  // Not interruptible by buttons: the accelerated run models an unattended overnight sleep.
  const uint64_t waitMicros = sleepDelayMicros / divisor;
  std::this_thread::sleep_for(std::chrono::microseconds(waitMicros < 1000 ? 1000 : waitMicros));
  if (bounded) setenv(MAX_WAKES_ENV, std::to_string(wakesLeft - 1).c_str(), 1);
  applyNextHeapEntry();
  for (const auto& names : AFTER_WAKE_ENVS) moveEnv(names[0], names[1]);
  setenv(TIMER_WAKE_ENV, "1", 1);
  // Fresh process, same host SD directory: the deep-sleep reset as far as firmware state goes.
  SimulatorLifecycle::rebootAsPowerWake();
}
