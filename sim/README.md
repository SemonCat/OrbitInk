# `sim/` — CrossFade's owned simulator adapters

Everything here exists only for `[env:simulator_x3]`. No device build sees it: the environment
declares it as an ordinary per-environment dependency,
`CrossFadeSimHal=symlink://sim/CrossFadeSimHal`, the same idiom `platformio.ini` already uses for
every `freeink-sdk/libs/...` entry. That is precisely why this directory is not under `lib/` — a
library there would be visible to every build and would have to be excluded by hand from each one.

Developer-facing usage lives in [`docs/contributing/simulator.md`](../docs/contributing/simulator.md).
This file is about *why the code here is shaped the way it is*, for whoever has to change it next.

## The problem this solves

[crosspoint-simulator](https://github.com/0x1abin/crosspoint-simulator) does not sit *under* the
firmware HAL — it **replaces** it. `lib_ignore = hal` drops all of `lib/hal/`, and the simulator
supplies `HalDisplay`, `HalStorage`, `HalGPIO`, `HalClock`, `HalSystem`, `HalPowerManager`,
`HalTiltSensor` and the Arduino/ESP-IDF emulation in its place. The simulator is therefore coupled
to one firmware's HAL by construction, and its own `FORKING.md` says so: if your fork changes a HAL
signature or adds a method, fork the simulator too.

CrossFade has diverged in exactly that way. The upstream simulator's own `main` does not build
against it at all, and even the CrossMux fork we pin is missing the CrossFade-only surface. Since
the pin is fixed and the checkout under `.pio/libdeps/` must never be patched (PlatformIO owns that
directory and will discard edits on the next refetch), the difference is carried here instead.

## What is in here, and which mechanism each file uses

| File | Mechanism | Why |
|---|---|---|
| `CrossFadeSimHal/CalendarTime.h` | forwarder | `lib_ignore = hal` also removes `lib/hal/CalendarTime.h`, which is pure `constexpr` calendar arithmetic with no hardware in it. Re-exports the production header, so there is one definition and it cannot drift. |
| `CrossFadeSimHal/HalSystem.h` | `#include_next` + declaration | `HalSystem` is a namespace, so `recordBootIdentity()` can simply be declared alongside the simulator's own declarations. Nothing is copied. |
| `CrossFadeSimHal/esp_ota_ops.h` | `#include_next` + additions | Adds the OTA rollback *state query* (`esp_ota_get_running_partition`, `esp_ota_get_state_partition`, `esp_ota_img_states_t`) the pinned simulator lacks. Free functions and a new type, so purely additive. |
| `CrossFadeSimHal/SecureHttpClient.h` | full replacement | No simulator translation unit includes this header, so replacing it outright leaves exactly one definition in the program. |
| `CrossFadeSimHal/HalClock.h` | **superset shadow** | Adds `getEpochMinutes()` to a class. |
| `CrossFadeSimHal/HalStorage.h` | **superset shadow** | Adds `PathStatus`/`probePath()` to `HalStorage` and `getModifyTime()`/`getError()` to `HalFile`. |

The `*Compat.cpp` files hold the definitions for the added members. Each one is written against the
simulator's **public** API only — never its private state — so the implementations stay correct
regardless of how the simulator stores things internally.

## The superset shadows, and the rule that keeps them safe

A member function cannot be added to a class from outside it. For `HalClock` and `HalStorage` the
only options were to patch the dependency (forbidden), to change production firmware source
(out of scope, and it would put `#ifdef SIMULATOR` into files that have nothing to do with the
simulator), or to declare the class ourselves. This directory takes the third.

Each shadow **mirrors the pinned commit's header declaration for declaration, plus CrossFade's
additions at the end of the class**. It is reformatted to this repo's clang-format style rather than
copied byte for byte (the formatter would rewrite it anyway), so re-checking one is a reading task,
not a `diff` invocation: what must match is the member list, its order, and every inline body.

The simulator's own `.cpp` files include their copy through a quoted include, which always resolves
to their own directory first, so they keep compiling against the original. The shadow is what the
*firmware* sees, because this library's directory precedes
`.pio/libdeps/simulator_x3/simulator/src/` in the include search order.

That ordering is not a documented PlatformIO guarantee, but it fails safely: if it ever inverted,
every call site would stop resolving and the build would fail immediately with "no member named
`probePath`" — there is no arrangement that links and then misbehaves. The fix if that happens is a
`pre:` extra script calling `env.Prepend(CPPPATH=...)`; don't add one before it is needed.

Two definitions of the same class therefore exist in one program. That is well-defined in practice
only while they agree on data layout and on every inline body, because the objects themselves
(`halClock`, `HalStorage::instance`) are constructed in the *simulator's* translation units.

**The exact dependency pin in `platformio.ini` is what makes that hold.** Nothing upstream can move
underneath us. So:

> **When you bump the simulator pin, re-check `HalClock.h` and `HalStorage.h` against the new
> `src/HalClock.h` and `src/HalStorage.h` before anything else.** Additions go after the marker
> comment; everything above it must still match upstream declaration for declaration.

Each shadow also carries a `static_assert` on its own `sizeof`. That does not detect upstream drift
— this code never sees the simulator's header — but it does catch the mistake most likely to
actually happen: "completing" a shadow from `lib/hal/` instead of from the pinned simulator.
`lib/hal/HalClock.h` embeds a FreeInk SDK `Rtc` by value plus five cache fields and is a completely
different object; without the assert, that copy would compile, link, and then read past the end of
the `halClock` the simulator allocated. A build-time script diffing each mirrored region against the
pinned checkout would close the upstream-drift gap too — worth adding if this layer grows, not worth
it at two shadows.

Two habits keep this cheap:

- **Never define an added member inline here.** Declare it in the header, define it in the matching
  `*Compat.cpp`. An inline body compiled against this file's view of the private data would be the
  one thing that could silently read the wrong offset.
- **Add at the end of the class, never in the middle**, and never add a `virtual`. Appending
  non-virtual members changes neither layout nor vtable.

If a piece of this divergence ever lands in upstream CrossPoint, the simulator will track it and
the corresponding shadow can be deleted. The `#include_next` and forwarder files are the shapes to
prefer whenever a new gap allows them; a shadow is the last resort.

## Deliberate fidelity gaps

These are simplifications, not oversights, and each is commented at its definition:

- **Networking is offline unless explicitly enabled.** The owned `SecureHttpClient` adapter can
  invoke an absolute `CROSSFADE_SIM_HTTP_BRIDGE` executable for scoped acceptance runs. The
  production sync policy, EPUB mapper and client are unchanged; host transport and provisioned
  authentication are supplied by the bridge. See [BOOKORBIT_SYNC.md](BOOKORBIT_SYNC.md).
  The `FREEINK_NET_WOLFSSL=1` flag still keeps downloader traffic on this adapter, not the
  dependency's separate curl transport. Device TLS, WiFi and memory behavior are not emulated.
- **`probePath()` never reports an I/O failure.** It mirrors the device's `Error` for a null/empty
  path, but the host filesystem has no partial-enumeration failure, so for real paths
  `Present`/`Absent` is exact and the callers' I/O-error branches stay device-only.
- **`getModifyTime()` reports no timestamp**, so `LibraryScanner` falls back to size-only change
  detection — the same fallback the device already uses for files this firmware wrote itself.
- **`recordBootIdentity()` is a no-op**, so the panic-recovery boot path cannot be reached here.

Anything in that list has to be verified on device or through the host gtest suites under `test/`.
The simulator is for UI iteration, not for these.
