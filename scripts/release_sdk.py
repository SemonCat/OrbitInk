"""Controlled, project-owned pioarduino SDK cache. No global package writes.

Installed packages are seed inputs, never evidence of a successful SDK build.
Only a successful cold driver build plus artifact verification can seal a manifest.
"""
from __future__ import annotations

import configparser
import hashlib
import inspect
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ENV = "x3_zh_tw_custom"
SCHEMA = 1


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class DigestCache:
    """Accidental-corruption model: reuse SHA only for an unchanged full stat tuple."""
    def __init__(self, path):
        self.path = Path(path)
        try:
            data = json.loads(self.path.read_text())
            if not isinstance(data, dict):
                data = {}
            self.entries = (data["entries"] if data.get("schema") == 1
                            and data.get("checksum") == digest(data.get("entries")) else {})
            if not isinstance(self.entries, dict):
                self.entries = {}
        except (OSError, ValueError, KeyError, TypeError):
            self.entries = {}
        self.used = set()

    @staticmethod
    def signature(path):
        stat = path.stat()
        return [stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns]

    def hash(self, path):
        path = Path(path).absolute()
        key, before = str(path), self.signature(path)
        self.used.add(key)
        cached = self.entries.get(key)
        if (isinstance(cached, dict) and cached.get("stat") == before
                and isinstance(cached.get("sha256"), str)
                and re.fullmatch(r"[0-9a-f]{64}", cached["sha256"])):
            return cached["sha256"]
        value = sha(path)
        if self.signature(path) != before:
            raise RuntimeError(f"File changed while computing SDK digest: {path}")
        self.entries[key] = {"stat": before, "sha256": value}
        return value

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        entries = {key: self.entries[key] for key in sorted(self.used)}
        temporary.write_text(json.dumps({"schema": 1, "entries": entries,
                                         "checksum": digest(entries)}) + "\n")
        temporary.replace(self.path)


def inventory(root, digests=None):
    """Always enumerate the complete file set and link targets, even on cache hits."""
    root = Path(root)
    hash_file = digests.hash if digests else sha
    files = {str(p.relative_to(root)): hash_file(p) for p in sorted(root.rglob("*"))
            if p.is_file() and not any(x in {".git", "__pycache__"} for x in p.parts)
            and p.suffix != ".pyc"}
    files.update({str(p.relative_to(root)) + "@link": os.readlink(p)
                  for p in root.rglob("*") if p.is_symlink()})
    return files


def protected_command(command, roots):
    """The macOS kernel enforces read-only shared tools, including subprocesses."""
    if sys.platform != "darwin":
        return command  # Other hosts use physical copies instead of shared packages.
    profile = '(version 1)(allow default)' + ''.join(
        f'(deny file-write* (subpath {json.dumps(str(Path(root).resolve()))}))'
        for root in roots)
    return ["/usr/bin/sandbox-exec", "-p", profile, *command]


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def write_changed(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != value:
        path.write_text(value)


def effective_config(root, pio):
    output = subprocess.check_output([pio, "project", "config", "--json-output"],
                                     cwd=root, text=True)
    return {section: dict(options) for section, options in json.loads(output)}


def sdk_options(config):
    # The SDK consumes board/ABI/custom Kconfig/component and compiler options.
    # Only the product runtime version is intentionally excluded from SDK identity.
    options = dict(config[f"env:{ENV}"])
    for key in list(options):
        if key in {"extends", "extra_scripts", "lib_deps", "lib_ignore"} or key.startswith(("monitor_", "upload_", "check_")):
            del options[key]
    flags = options.get("build_flags", [])
    if isinstance(flags, str):
        flags = flags.splitlines()
    options["build_flags"] = [flag for flag in flags
                              if "CROSSPOINT_VERSION" not in flag and "-Wl," not in flag]
    return options


def ini_text(options):
    parser = configparser.ConfigParser(interpolation=None)
    parser["platformio"] = {"default_envs": ENV}
    parser[f"env:{ENV}"] = {key: "\n".join(value) if isinstance(value, list) else str(value)
                            for key, value in options.items()}
    import io
    stream = io.StringIO()
    parser.write(stream)
    return stream.getvalue()


def trusted(manifest, key, artifacts):
    return (isinstance(manifest, dict) and manifest.get("schema") == SCHEMA
            and manifest.get("key") == key and manifest.get("controlled_build") is True
            and bool(artifacts) and manifest.get("artifacts") == artifacts)


class SdkCache:
    def __init__(self, root, pio, config):
        self.root, self.pio, self.config = Path(root), pio, config
        self.state = self.root / ".release-sdk"
        # pioarduino's unmodified cleanup gate requires this path component.
        self.core = self.state / ".platformio"
        self.project = self.state / "project"
        self.manifest_path = self.state / "manifest.json"
        self.seed = Path.home() / ".platformio"
        self.digests = DigestCache(self.state / "digests.json")

    def inputs(self):
        options = sdk_options(self.config)
        # These inputs may be ignored/local; Git cleanliness alone cannot guard them.
        configs = {str(p.relative_to(self.root)): self.digests.hash(p)
                   for p in sorted(self.root.glob("sdkconfig*")) if p.is_file()}
        partitions = options.get("board_build.partitions", "partitions.csv")
        return {"schema": SCHEMA, "options": options, "sdkconfig_inputs": configs,
                "partitions": self.digests.hash(self.root / partitions),
                # Cache implementation changes are not SDK recipe changes.
                "recipe": digest([inspect.getsource(function) for function in
                                  (sdk_options, ini_text, self.prepare_cold, self.environment)]),
                "pio": subprocess.check_output([self.pio, "--version"], text=True).strip(),
                "platform": inventory(self.seed / "platforms" / "espressif32", self.digests),
                "packages": self.seed_packages()}

    def seed_packages(self):
        result = {}
        for package in sorted((self.seed / "packages").iterdir()):
            if not package.is_dir():
                continue
            if package.name == "framework-arduinoespressif32-libs":
                files = {p.name: self.digests.hash(p) for p in package.iterdir() if p.is_file()}
                files.update({"esp32c3/" + name: value
                              for name, value in inventory(package / "esp32c3", self.digests).items()})
            else:
                files = inventory(package, self.digests)
            result[package.name] = files
        return result

    def artifacts(self):
        return {"platform": inventory(self.core / "platforms", self.digests),
                "packages": inventory(self.core / "packages", self.digests),
                "defaults": self.digests.hash(self.project / "sdkconfig.defaults")
                if (self.project / "sdkconfig.defaults").is_file() else None,
                "sdkconfig": self.digests.hash(self.project / f"sdkconfig.{ENV}")
                if (self.project / f"sdkconfig.{ENV}").is_file() else None}

    def load(self):
        try:
            return json.loads(self.manifest_path.read_text())
        except (OSError, ValueError):
            return None

    def environment(self):
        # Do not allow caller overrides to route writes back into global packages.
        env = {k: v for k, v in os.environ.items() if not k.startswith("PLATFORMIO_")}
        env.update(PLATFORMIO_CORE_DIR=str(self.core),
                   PLATFORMIO_PACKAGES_DIR=str(self.core / "packages"),
                   PLATFORMIO_PLATFORMS_DIR=str(self.core / "platforms"),
                   SOURCE_DATE_EPOCH="0", TZ="UTC", PYTHONDONTWRITEBYTECODE="1")
        return env

    def prepare_cold(self):
        self.manifest_path.unlink(missing_ok=True)
        # All deletion is confined to dedicated generated state; never root scaffolds.
        for path in (self.core / "packages", self.core / "platforms", self.core / "tools",
                     self.project, self.state / "scons-cache"):
            if path.is_symlink():
                raise RuntimeError(f"Refusing symlink state directory: {path}")
            if path.exists():
                shutil.rmtree(path)
        self.core.mkdir(parents=True, exist_ok=True)
        packages = self.core / "packages"
        packages.mkdir()
        shutil.copytree(self.seed / "platforms", self.core / "platforms")
        for source in (self.seed / "packages").iterdir():
            if not source.is_dir():
                continue
            destination = packages / source.name
            if source.name == "framework-arduinoespressif32-libs":
                # Only the fixed X3 target is an SDK input/output for this release.
                def ignore(path, names):
                    return [name for name in names if Path(path) == source
                            and name.startswith("esp32") and name != "esp32c3"]
                shutil.copytree(source, destination, ignore=ignore)
                # With no custom-output stamp, the upstream builder's fresh-SDK
                # branch compiles all configured components without reinstalling
                # the full multi-target Arduino package. This copy remains untrusted.
                (destination / "sdkconfig").unlink(missing_ok=True)
            elif source.name == "framework-arduinoespressif32" or sys.platform != "darwin":
                shutil.copytree(source, destination)
            else:
                destination.symlink_to(source, target_is_directory=True)
        self.project.mkdir()
        for name in ("scripts", "lib", "include", "freeink-sdk"):
            if (self.root / name).exists():
                (self.project / name).symlink_to(self.root / name, target_is_directory=True)
        shutil.copy2(self.root / "partitions.csv", self.project / "partitions.csv")
        (self.project / "src").mkdir()
        (self.project / "src" / "main.cpp").write_text("#include <Arduino.h>\nvoid setup() {}\nvoid loop() {}\n")
        options = sdk_options(self.config)
        write_changed(self.project / "platformio.ini", ini_text(options))

    def prepare_app(self):
        src = self.project / "src"
        if src.exists() and not src.is_symlink():
            shutil.rmtree(src)
        if not src.exists():
            src.symlink_to(self.root / "src", target_is_directory=True)
        options = dict(self.config[f"env:{ENV}"])
        options.pop("extends", None)
        write_changed(self.project / "platformio.ini", ini_text(options))
        shutil.copy2(self.root / "partitions.csv", self.project / "partitions.csv")

    def seal(self, key, inputs):
        artifacts = self.artifacts()
        libs = self.core / "packages/framework-arduinoespressif32-libs/esp32c3"
        required = [libs / "lib/libesp_app_format.a", libs / "ld/sections.ld",
                    libs / "dio_qspi/include/sdkconfig.h"]
        if not all(p.is_file() for p in required) or not artifacts["defaults"] or not artifacts["sdkconfig"]:
            raise RuntimeError("Controlled SDK build did not produce complete artifacts")
        data = {"schema": SCHEMA, "key": key, "inputs": inputs,
                "controlled_build": True, "artifacts": artifacts}
        temporary = self.manifest_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + "\n")
        temporary.replace(self.manifest_path)
