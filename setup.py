"""Build the game binding against the one shared native game library it installs."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import sysconfig

from Cython.Build import cythonize
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT = Path(__file__).resolve().parent


def macos_target() -> str:
    """The interpreter's (or the environment's) deployment target, raised to 10.15: the
    first macOS whose libc++ ships the `std::filesystem` the public API takes."""
    target = os.environ.get("MACOSX_DEPLOYMENT_TARGET") or sysconfig.get_config_var("MACOSX_DEPLOYMENT_TARGET")
    return target if tuple(map(int, target.split("."))) >= (10, 15) else "10.15"


MACOS_TARGET = macos_target() if sys.platform == "darwin" else None


def build_native() -> Path:
    """Configure, build and install the shared `splendor::game` library; returns the
    install prefix. The build directory is keyed by the toolchain environment."""
    import cmake

    toolchain = {name: os.environ.get(name, "") for name in (
        "CC", "CXX", "CPPFLAGS", "CXXFLAGS", "ARCHFLAGS", "MACOSX_DEPLOYMENT_TARGET",
        "SPLENDOR_GAME_SEED_NAMESPACE",
    )}
    key = hashlib.sha256(json.dumps(toolchain, sort_keys=True).encode()).hexdigest()[:12]
    build = ROOT / "build" / f"python-{key}"
    args = [
        "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DBUILD_SHARED_LIBS=ON",
        "-DSPLENDOR_GAME_BUILD_TESTS=OFF", "-DSPLENDOR_GAME_INSTALL=ON",
        "-DCMAKE_INSTALL_LIBDIR=lib",
        f"-DCMAKE_CXX_FLAGS={toolchain['CPPFLAGS']} {toolchain['CXXFLAGS']}",
    ]
    if toolchain["SPLENDOR_GAME_SEED_NAMESPACE"]:
        # A downstream's own seed-law tag (CMakeLists.txt, SPLENDOR_GAME_SEED_NAMESPACE).
        args.append(f"-DSPLENDOR_GAME_SEED_NAMESPACE={toolchain['SPLENDOR_GAME_SEED_NAMESPACE']}")
    if sys.platform == "darwin":
        archs = re.findall(r"-arch\s+(\S+)", toolchain["ARCHFLAGS"])
        if not archs:
            arch = sysconfig.get_platform().rsplit("-", 1)[-1]
            archs = ["arm64", "x86_64"] if arch == "universal2" else [arch]
        args += [f"-DCMAKE_OSX_ARCHITECTURES={';'.join(archs)}", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_TARGET}"]
    binary = str(Path(cmake.CMAKE_BIN_DIR) / "cmake")
    prefix = build / "install"
    subprocess.run([binary, *args], check=True)
    subprocess.run([binary, "--build", str(build), "--parallel", str(os.cpu_count() or 1)], check=True)
    subprocess.run([binary, "--install", str(build), "--prefix", str(prefix)], check=True)
    return prefix


class GameBuildExt(build_ext):
    def build_extensions(self):
        prefix = build_native()
        for ext in self.extensions:
            ext.include_dirs = [str(prefix / "include")]
            ext.library_dirs = [str(prefix / "lib")]
        # Native sources are not visible to setuptools timestamp checks.
        self.force = self.compiler.force = True
        super().build_extensions()
        shutil.copytree(prefix, Path(self.build_lib) / "splendor_game/_core", dirs_exist_ok=True)

    def copy_extensions_to_source(self):
        super().copy_extensions_to_source()
        shutil.copytree(
            Path(self.build_lib) / "splendor_game/_core",
            ROOT / "python/splendor_game/_core", dirs_exist_ok=True,
        )


def _extension(name: str, source: str, libraries: list[str], core: str) -> Extension:
    # After the interpreter's own flags, so its older `-mmacosx-version-min` is overridden.
    macos = [f"-mmacosx-version-min={MACOS_TARGET}"] if MACOS_TARGET else []
    return Extension(
        name, [source], libraries=libraries,
        runtime_library_dirs=[f"@loader_path/{core}" if sys.platform == "darwin" else f"$ORIGIN/{core}"],
        language="c++", extra_compile_args=["-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic", *macos],
        extra_link_args=macos,
    )


setup(
    ext_modules=cythonize([
        # Its board and result renderings are the play library's.
        _extension("splendor_game.engine", "python/splendor_game/engine.pyx",
                   ["splendor_play", "splendor_game"], "_core/lib"),
        # The match host: the play library over the same shared game library.
        _extension("splendor_play._native", "python/splendor_play/_native.pyx",
                   ["splendor_play", "splendor_game"], "../splendor_game/_core/lib"),
    ], compiler_directives={"language_level": 3}, include_path=[str(ROOT / "python")]),
    cmdclass={"build_ext": GameBuildExt},
)
