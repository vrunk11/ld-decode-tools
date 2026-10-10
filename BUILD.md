# Build (Nix)

This project uses a Nix dev shell to provide a consistent build environment.

## Enter the dev shell

```bash
nix develop
```

This exposes all build dependencies (CMake, Ninja, Qt6, FFmpeg, FFTW, SQLite, OpenGL, etc.).

## Configure and build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Artifacts will be under `build/`.

## Build without entering the shell (one-off)

```bash
nix develop -c cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nix develop -c ninja -C build
```

## Optional: clean build

```bash
rm -rf build
```

## Build scripts

`build.sh` (Linux/macOS, ideally inside `nix develop`) and `build.bat` (Windows) configure
and build into `build/` at the repository root:

```bash
./build.sh [Release|Debug|RelWithDebInfo] [extra cmake arguments]
```

Both scripts build for **this machine**: they set `ENABLE_NATIVE_OPTIMIZATION=ON`, which adds
link-time optimisation and `-march=native` (GCC/Clang; MSVC gets LTO only). The resulting
binaries may not run on an older or different CPU. Set `NATIVE=0` for a portable build;
Debug builds never use it. CI, Nix and the release packages leave the option off.

## Windows (vcpkg)

`build.bat` takes every dependency from a project-local vcpkg, the same way decode-orc does:

- `vcpkg/` is cloned and bootstrapped on first run (ignored by git). It must be a real
  `git clone`: vcpkg resolves the `builtin-baseline` in `vcpkg.json` from its own history.
- `vcpkg.json` lists FFTW3 and pkgconf, plus Qt 6 (qtbase + qtsvg) under the `qt` feature.
  They are installed into `build/vcpkg_installed/` during configure. **The first run builds
  Qt from source and takes a long time**; later runs reuse vcpkg's binary cache.
- The compiler is MSVC when `cl.exe` is on `PATH` (an "x64 Native Tools Command Prompt for
  VS"), otherwise MinGW-w64 gcc. The triplet follows (`x64-windows` / `x64-mingw-dynamic`)
  unless `VCPKG_TARGET_TRIPLET` is set.
- To use an installed Qt instead of building it: `set USE_SYSTEM_QT=1` and pass
  `-DCMAKE_PREFIX_PATH=C:/Qt/6.9.0/msvc2022_64`.

```bat
build.bat Release
```

CI's Windows package uses the same `vcpkg.json` without the `qt` feature, with an official Qt.

## Notes

- The flake sets `-DEZPWD_DIR`, `-DAPP_BRANCH`, and `-DAPP_COMMIT` automatically for package builds.
- The dev shell exports `EZPWD_DIR`, so manual builds via `nix develop` pick it up automatically.
