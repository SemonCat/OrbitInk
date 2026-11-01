#include "SleepTestHooks.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDL.h>

#include <cstdio>
#include <cstdlib>

extern HalDisplay display;

unsigned long crossfadeSimSleepTimeoutMs(const unsigned long configured) {
  if (!configured) return 0;
  const char* value = std::getenv("CROSSFADE_SIM_IDLE_TIMEOUT_MS");
  if (!value || !*value) return configured;
  char* end = nullptr;
  const unsigned long timeout = std::strtoul(value, &end, 10);
  return *end == '\0' && timeout >= 1000 && timeout <= configured ? timeout : configured;
}

// The ordinary screenshot scheduler stops during deep sleep. Capture the final SDL
// presentation here, on the simulator main thread, before its blocking sleep call.
void crossfadeSimCaptureSleep() {
  const char* path = std::getenv("CROSSFADE_SIM_SLEEP_CAPTURE");
  if (!path || path[0] != '/') return;
  display.presentIfNeeded();
  // Display initialization can create a second window during splash setup.
  SDL_Renderer* target = nullptr;
  for (Uint32 id = 1; id <= 16; ++id) {
    if (SDL_Window* window = SDL_GetWindowFromID(id)) {
      if (SDL_Renderer* candidate = SDL_GetRenderer(window)) target = candidate;
    }
  }
  int width = 0, height = 0;
  if (!target || SDL_GetRendererOutputSize(target, &width, &height) != 0) {
    std::fprintf(stderr, "[SIM] Sleep capture renderer unavailable: %s\n", SDL_GetError());
    return;
  }
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) return;
  if (SDL_RenderReadPixels(target, nullptr, surface->format->format, surface->pixels, surface->pitch) == 0 &&
      SDL_SaveBMP(surface, path) == 0) {
    std::fprintf(stderr, "[SIM] Saved sleep capture: %s\n", path);
  } else {
    std::fprintf(stderr, "[SIM] Sleep capture failed: %s\n", SDL_GetError());
  }
  SDL_FreeSurface(surface);
  unsetenv("CROSSFADE_SIM_SLEEP_CAPTURE");
}
