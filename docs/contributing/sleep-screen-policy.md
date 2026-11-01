# Sleep screen policy

Manual power-button sleep and inactivity sleep use the same selected Sleep
Screen mode. The trigger changes neither the screen nor the wake policy.

- Custom uses `/sleep.bmp` or the existing sleep-image directory.
- Cover uses the current/last book cover, with the existing default fallback.
- Cover + Custom uses the cover when leaving a reader, custom otherwise; a
  missing reader cover still falls back to custom.
- Quick Resume preserves the reader with the existing optional transparent
  overlay or moon fallback. Outside a reader it retains the default sleep screen.
- Transparent overlay preserves the outgoing view, including Home. At Home the
  temporary rollback frame is removed after rendering and wake uses normal Home
  startup; reader fast boot remains reader-only.

Dark, light and blank retain their existing behavior. Random wallpaper folders
may choose different images on successive sleeps; equal policy does not imply
the same random image.

The obsolete `quickResumeSleepScreen` ("Quick Resume on Timeout") key remains
readable and is saved unchanged for downgrade compatibility. It no longer
overrides the explicit Sleep Screen mode and is hidden from device/web settings.
Opening either settings UI no longer changes it automatically. No selected mode,
book, progress, image or other user data is deleted or migrated. Users who want
the old timeout-only last-page effect can explicitly select Quick Resume for both
triggers.

Sleep entry captures one `SleepScreenPolicy` before replacing the reader. The
same value controls framebuffer storage, boot flags and SleepActivity rendering.
Reader exit still flushes progress before the unchanged sleep sync interval;
failed progress persistence still invalidates fast-render eligibility.

## Simulator regression

Build `simulator_x3_zh_tw`, then run with the packaged fonts:

```sh
CROSSFADE_SIM_ZH_TW_FONTS=/path/to/sd-assets/fonts \
  uv run --with pillow python scripts/sim_unified_sleep_smoke.py --keep /absolute/artifacts
```

The owned host adapter supports `CROSSFADE_SIM_IDLE_TIMEOUT_MS` (at least 1000ms,
never longer than the configured timeout; Never remains disabled) and
`CROSSFADE_SIM_SLEEP_CAPTURE=/absolute/sleep.bmp`. The latter captures the actual
SDL sleep presentation before blocking, since the normal screenshot scheduler
cannot run inside deep sleep. These controls do not compile into device firmware.
Tests use one wallpaper/overlay, isolated SD cards and the offline host transport,
compare manual/idle pixels within the same context, and turn a reader page after
button wake. The simulator cannot prove panel waveforms, battery timing or physical
button debounce; those remain device checks.
