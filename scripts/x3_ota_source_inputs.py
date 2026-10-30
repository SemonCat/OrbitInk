"""Source inputs sealed by the guarded build and checked again before distribution."""

import hashlib
import io
import subprocess
import tarfile
from pathlib import Path

from release_sdk import ENV, digest, sha

PROJECT_PATHS = ["src", "lib", "scripts", "sim", "test", "platformio.ini", "partitions.csv",
                 "requirements.txt", "LICENSE", "LICENSES", "NOTICE", ".gitmodules", ".gitignore",
                 "docs/contributing/x3-release.md", "docs/contributing/x3-ota.md",
                 "docs/x3-zh-tw-custom.md", "README.md", ".github/workflows",
                 "docs/contributing/orbitink-channels.md", "docs/OrbitInk-README-zh-TW.md"]
EXCLUDED_DIRS = {".git", "__pycache__", ".pio", "build", "node_modules", ".cache"}
BINARY_SUFFIXES = {".a", ".o", ".elf", ".bin", ".pyc", ".exe", ".dll", ".so", ".dylib", ".pio-link"}


def source_files(root):
    """Owned Git libraries are captured separately; never follow arbitrary nested links."""
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if any(part in EXCLUDED_DIRS for part in relative.parts):
            continue
        if path.is_symlink() or not path.is_file() or path.suffix in BINARY_SUFFIXES:
            continue
        yield relative.as_posix(), path


def tracked_sources(root):
    """Inventory exactly the product/SDK Git members that will be distributed."""
    for cwd, prefix, paths in ((root, "project", PROJECT_PATHS),
                               (root / "freeink-sdk", "freeink-sdk", [])):
        archive = subprocess.check_output(["git", "archive", "HEAD", *paths], cwd=cwd)
        with tarfile.open(fileobj=io.BytesIO(archive)) as tracked:
            for member in tracked:
                if member.isfile():
                    yield f"{prefix}/{member.name}", tracked.extractfile(member).read()


def input_files(cache):
    roots = {
        "dependencies": cache.project / f".pio/libdeps/{ENV}",
        "managed_components": cache.project / "managed_components",
    }
    for package in ("framework-arduinoespressif32", "framework-arduinoespressif32-libs",
                    "framework-espidf"):
        roots[f"packages/{package}"] = cache.core / "packages" / package
    files = {}
    for prefix, root in roots.items():
        if not root.is_dir():
            raise ValueError(f"missing built source input: {root}")
        for name, path in source_files(root.resolve()):
            files[f"{prefix}/{name}"] = path

    for name in ("platformio.ini", "sdkconfig.defaults", f"sdkconfig.{ENV}", "partitions.csv"):
        path = cache.project / name
        if not path.is_file():
            raise ValueError(f"missing effective build input: {path}")
        files[f"build-inputs/project/{name}"] = path
    files["build-inputs/manifest.json"] = cache.manifest_path

    # The effective INI can refer to locally overridden scripts/data. Archive the actual
    # referenced bytes too; do not silently pull arbitrary paths outside this checkout.
    options = cache.config[f"env:{ENV}"]
    for option in ("extra_scripts", "board_build.partitions", "board_build.embed_txtfiles",
                   "board_build.embed_files"):
        values = options.get(option, [])
        if isinstance(values, str):
            values = values.splitlines()
        for value in values:
            name = value.strip()
            if not name:
                continue
            if option == "extra_scripts" and name.startswith(("pre:", "post:")):
                name = name.split(":", 1)[1]
            relative = Path(name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"unsupported source reference in {option}: {name}")
            path = cache.project / relative
            if not path.is_file() or not path.resolve().is_relative_to(cache.root.resolve()):
                raise ValueError(f"source reference is missing or outside checkout: {name}")
            files[f"build-inputs/project/{relative.as_posix()}"] = path
    return files


def capture(cache):
    files = {name: sha(path) for name, path in sorted(input_files(cache).items())}
    files.update({name: hashlib.sha256(data).hexdigest() for name, data in tracked_sources(cache.root)})
    return {
        "schema": 1,
        "files": files,
        "sdk_artifacts_sha256": digest(cache.artifacts()),
    }


def verify(cache, expected):
    if not isinstance(expected, dict) or expected.get("schema") != 1 or capture(cache) != expected:
        raise ValueError("source inputs or SDK artifacts differ from the guarded release")
