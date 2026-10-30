# X3 zh-TW releases

Run `python3 scripts/release_x3_zh_tw.py` from a clean checkout. The only
untracked exception is `test/build_host/`; tracked changes there still block.
Use `--cold` to recover the private SDK cache. No push or publication occurs.

The script keeps only mutable Arduino framework sources and ESP32-C3 SDK
outputs in `.release-sdk/.platformio/packages/`. On macOS, other installed
packages (including the toolchain and ESP-IDF source) are shared through
symlinks. The native `sandbox-exec` kernel policy denies writes to
`~/.platformio` and the submodule, including writes through those symlinks.
Other hosts use physical package copies. A separate private Python tool
environment is retained across cold builds; no full tools mirror is needed.

Copied SDK files are **untrusted seed inputs**. Removing only the private
custom-output stamp selects pioarduino's fresh-SDK compilation branch, avoiding
reinstallation/download of its entire multi-target package. A minimal Arduino
driver must successfully compile and copy the configured ESP-IDF archives
before a manifest can be recorded. Normal warm PlatformIO builds write the
Arduino build recipe even when they restore it afterward; using the global
mutable framework directly would violate the no-third-party-writes contract.
The app project uses symlinks to owned sources and the existing submodule;
generated scaffold, libraries, build output and logs remain under
`.release-sdk/`. Do not run another build against that private project.
SCons uses its normal build-directory signature database; no second object
cache is configured.

The SDK key covers effective SDK/board/compiler options (excluding only the
product version and app link wrapper flags), partition contents, root
`sdkconfig*` inputs, the SDK recipe, PlatformIO core version, and content hashes
of the installed platform and packages, including frameworks and toolchain.
Root `sdkconfig*` files are monitored/invalidation inputs; the controlled SDK
driver generates its effective sdkconfig from `custom_sdkconfig`, as the
pioarduino builder does on a fresh project. Its resulting defaults/sdkconfig,
private platform and complete package file inventory are sealed and rehashed
before reuse. A missing/incomplete manifest or changed key/artifact forces a
cold build. The manifest is a local integrity/provenance record, not a signed
attestation against an attacker who can replace both manifest and artifacts.
The digest cache also has an integrity checksum. Inventories enumerate files
and symlink targets on every check, reusing SHA-256 only when path, device,
inode, size, nanosecond mtime and nanosecond ctime agree. New or changed files
are rehashed. This models accidental stale/corrupt build state, not an attacker
who can preserve all filesystem metadata. Cache implementation changes are
separate from the fingerprint of the actual SDK build recipe.

`project_app_descriptor.py` compiles one project C object against the installed
ESP-IDF header. ABI static assertions, the strong ELF symbol, image offset 32,
unique descriptor magic, full Git HEAD marker, exact Git describe, runtime
version and ELF SHA256 are independent release checks. No bytes of the ELF or
firmware image are patched. IDF 5.5.2's archive symbol is weak; linker garbage
collection must remove its superseded descriptor section. The final image
guard refuses a future toolchain/SDK that does not do so.

HEAD or tags changing rewrites only the generated descriptor C source. Product
source/version changes follow ordinary SCons dependencies. SDK identity does
not contain HEAD, Git describe or product version. A watcher plus final checks
reject root HEAD/config/cleanliness/submodule changes during the build. The
submodule's recorded pointer and actual checkout must agree. Exact tags and
detached HEAD are supported; descriptions over the ABI's 31-byte limit fail.

Tar/gzip timestamps, modes, owners and member order are fixed. The project
firmware-update log uses the product version instead of compilation time, and
generated web gzip data uses mtime zero. SDK compilation uses a fixed
`SOURCE_DATE_EPOCH=0`. Repeated warm releases should be byte-identical. This
does not promise reproducible ELF debug paths across different checkout paths,
host/Python/tool versions, or separately downloaded upstream packages.
The wolfSSL patcher, HTML generator and i18n generator compare output bytes
before writing; JPEGDEC checks whether each patch is already applied. Repeated
pre-build runs preserve generated-file mtimes as well as contents.

Validation:

```sh
python3 -m unittest discover -s scripts -p 'test_release*.py'
cmake --build test/build_host -j 8
ctest --test-dir test/build_host --output-on-failure
python3 scripts/release_x3_zh_tw.py --cold
python3 scripts/release_x3_zh_tw.py
```

`.release-sdk/last-release.json` records identity, firmware/tar hashes,
RAM/Flash and elapsed times. `cold.log` and `app.log` retain compiler evidence.
The release artifact is under `dist/`; Ruby owns hardware acceptance and any
subsequent push/merge/publication.
