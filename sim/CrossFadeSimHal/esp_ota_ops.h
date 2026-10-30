#pragma once

// Owned simulator adapter: additive extension of crosspoint-simulator's esp_ota_ops shim.
//
// Purely additive on top of the dependency's own header (#include_next below), so nothing here can
// conflict with it. The pinned simulator stubs esp_ota_get_next_update_partition() and
// esp_ota_mark_app_valid_cancel_rollback() but not the rollback *state query* that
// src/main.cpp:546-549 uses to decide whether this slot is even pending verification:
// esp_ota_get_running_partition(), esp_ota_get_state_partition(), and esp_ota_img_states_t.
//
// This is ESP-IDF surface, not CrossFade HAL surface -- crosspoint-simulator's FORKING.md calls
// gaps in the emulated ESP-IDF/Arduino layer upstream bugs and asks for them as PRs. Worth
// upstreaming; kept here meanwhile because the dependency is pinned and never patched in place.
#include_next <esp_ota_ops.h>

// Values mirror ESP-IDF's esp_ota_ops.h so any firmware comparison keeps its meaning.
typedef enum {
  ESP_OTA_IMG_NEW = 0x0U,
  ESP_OTA_IMG_PENDING_VERIFY = 0x1U,
  ESP_OTA_IMG_VALID = 0x2U,
  ESP_OTA_IMG_INVALID = 0x3U,
  ESP_OTA_IMG_ABORTED = 0x4U,
  ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFFU,
} esp_ota_img_states_t;

// A host build has no partition table and no otadata: there is no image to confirm and no
// bootloader to roll back. Reporting "no running partition" makes src/main.cpp short-circuit
// before esp_ota_mark_app_valid_cancel_rollback(), which is the honest outcome -- the simulator
// must not appear to confirm an OTA slot.
inline const esp_partition_t* esp_ota_get_running_partition() { return nullptr; }

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*) {
  return ESP_FAIL;
}
