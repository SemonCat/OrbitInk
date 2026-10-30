"""Project descriptor: normal compiler/linker inputs, never a firmware patch."""
import json
import os
import subprocess
from pathlib import Path


def descriptor_source(head, describe, idf_version):
    if len(describe.encode()) > 31 or len(idf_version.encode()) > 31:
        raise ValueError("ESP app descriptor version exceeds its 31-byte ABI limit")
    return f'''#include "esp_app_desc.h"
#include "sdkconfig.h"
#ifndef CONFIG_BOOTLOADER_APP_SECURE_VERSION
#define CONFIG_BOOTLOADER_APP_SECURE_VERSION 0
#endif
_Static_assert(sizeof(esp_app_desc_t) == 256, "unsupported descriptor ABI");
_Static_assert(offsetof(esp_app_desc_t, version) == 16, "version ABI");
_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) == 144, "ELF SHA ABI");
_Static_assert(offsetof(esp_app_desc_t, mmu_page_size) == 180, "MMU ABI");
const esp_app_desc_t esp_app_desc __attribute__((section(".rodata_desc"), used)) = {{
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .secure_version = CONFIG_BOOTLOADER_APP_SECURE_VERSION,
    .version = {json.dumps(describe)},
    .project_name = "crossfade",
    .idf_ver = {json.dumps(idf_version)},
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    .mmu_page_size = 31 - __builtin_clz(CONFIG_MMU_PAGE_SIZE),
}};
const char crossfade_release_head[] = "CrossFade-HEAD:{head}";
'''


def install(env):
    if env["PIOENV"] != "x3_zh_tw_custom":
        return
    root = Path(os.environ.get("CROSSFADE_RELEASE_ROOT", env["PROJECT_DIR"]))
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root, text=True).strip()
    head = git("rev-parse", "HEAD")
    describe = git("describe", "--always", "--tags", "--dirty")
    if os.environ.get("CROSSFADE_RELEASE_HEAD", head) != head:
        raise RuntimeError("Root HEAD changed before descriptor compilation")
    platform = env.PioPlatform()
    idf = Path(platform.get_package_dir("framework-espidf"))
    source = descriptor_source(head, describe, (idf / "version.txt").read_text().strip())
    directory = Path(env.subst("$BUILD_DIR")) / "project_descriptor_source"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "project_app_descriptor.c"
    if not path.exists() or path.read_text() != source:
        path.write_text(source)
    env.Append(LINKFLAGS=["-Wl,-u,crossfade_release_head"])
    # Post scripts run after BuildProgram. A library is supported here and the
    # full-HEAD -u root guarantees this object is extracted even if the SDK's
    # weak descriptor was encountered first.
    env.Prepend(LIBS=[env.BuildLibrary("$BUILD_DIR/project_descriptor", str(directory))])


if "Import" in globals():
    Import("env")
    install(env)
