# AGENTS.md

Conventions for anyone — human or automated assistant — working in this repository.

Everything here applies to agents *and* to people. Where a rule exists specifically because
assistants get it wrong by default, that is called out.

---

## Rule 1 — No automatic git operations

**Never run a command that changes repository state unless the user has explicitly asked for
that specific action in that specific message.**

> **Forbidden without an explicit request:** `git add`, `git commit`, `git push`,
> `git stash`, `git rebase`, `git reset`, `git revert`, `git merge`, `git cherry-pick`,
> `git tag`, `git branch -d`/`-D`, any `git checkout`/`git switch` that discards changes,
> `git clean`, `git submodule update`, and any `gh pr create` / `gh pr merge`.
>
> **Always permitted:** `git status`, `git log`, `git diff`, `git show`, `git blame`,
> `git ls-files`, `git grep` — anything read-only.

- **"Make this change" means edit the files.** It does not imply staging or committing.
- **Permission for one commit does not carry to the next.** Each one is asked for separately.
- **Finishing a task is not a trigger to commit.** Leave the work in the working tree and
  report what changed.
- A dirty working tree at the end of a task is the expected outcome, not a loose end.

This repository was split out of `ld-decode` with `git-filter-repo`; an unrequested rewrite
or push here is expensive to unpick.

## Rule 2 — No AI attribution anywhere

**Nothing this project produces may advertise the tools used to produce it.**

> **Forbidden** in commit messages, PR titles and bodies, code comments, documentation and
> release notes:
>
> - `Co-Authored-By:` naming an AI tool or service
> - `Generated with …`, `Created by …`, `Written with the help of …`
> - `🤖`, "AI-assisted", or any tool or vendor name used as an attribution

Commit messages describe **the change**, not how it was written. Several assistants append
attribution trailers by default unless told not to — this rule is that instruction.

The same restriction covers advertisements, promotions and commercial references generally.

---

## 1. What this project is

**ld-decode-tools** is the suite of C++/Qt 6 tools that process the TBC (time-base corrected)
output of [ld-decode](https://github.com/happycube/ld-decode): analysis, VBI/VITS processing,
dropout correction, chroma decoding, disc stacking/mapping, metadata export and the EFM
(digital audio / data) decoder chain.

The project is **actively maintained**: fixes, compatibility work, restoring behaviour lost in
the 2025–2026 changes, and new features are all in scope. Keep existing captures, metadata
and command lines working (see §2 and §10).

- **Type:** C++17, Qt 6 (Core, Gui, Widgets, Sql, Svg), FFTW3
- **Build system:** CMake 3.16+, Ninja
- **Reproducible environment:** Nix flake (`flake.nix`) — authoritative for development and CI
- **Packages:** Nix, Linux tarball + AppImage, macOS DMG, Windows ZIP (built by CI)

### 1.1 Tools

| Directory | Binaries | Purpose |
| --- | --- | --- |
| `src/ld-analyse/` | `ld-analyse` | Qt GUI: TBC inspection, scopes, SNR/dropout analysis |
| `src/ld-chroma-decoder/` | `ld-chroma-decoder`, `ld-chroma-encoder` | PAL/NTSC colour decoding to RGB/YUV; test encoder |
| `src/ld-process-vbi/` | `ld-process-vbi` | Decode VBI (frame numbers, timecodes, VITC, closed captions) |
| `src/ld-process-vits/` | `ld-process-vits` | Vertical interval test signal measurements |
| `src/ld-process-efm/` | `ld-process-efm` | Single-step EFM decoding (digital audio or data) |
| `src/ld-process-ac3/` | `ld-ac3-demodulate`, `ld-ac3-decode` | AC3 (Dolby Digital) RF decoding; POSIX getopt (vcpkg on MSVC) |
| `src/ld-dropout-correct/` | `ld-dropout-correct` | Dropout concealment |
| `src/ld-disc-stacker/` | `ld-disc-stacker` | Combine several captures of one disc |
| `src/ld-discmap/` | `ld-discmap` | Map and repair TBC field order against VBI |
| `src/ld-export-metadata/`, `src/ld-export-decode-metadata/` | same | Export metadata to external formats |
| `src/ld-json-converter/` | `ld-json-converter` | JSON → SQLite metadata conversion |
| `src/ld-sqlite-to-json/` | `ld-sqlite-to-json` | SQLite → JSON metadata conversion |
| `src/ld-lds-converter/` | `ld-lds-converter` | 10-bit packed ↔ 16-bit sample conversion |
| `src/efm-decoder/` | `efm-decoder-{f2,d24,audio,data}`, `efm-stacker-f2`, `vfs-verifier` | Staged EFM decoding |
| `src/library/` | `lddecode-library` (static) | Shared TBC I/O, metadata (SQLite), VBI/VITC decoders, filters |

### 1.2 Repository layout

```
├── AGENTS.md                 # this file (CLAUDE.md and .github/copilot-instructions.md point here)
├── TESTING.md                # test slices and how to run them
├── CONTRIBUTING.md
├── BUILD.md / INSTALL.md     # Nix build and install
├── CMakeLists.txt            # top level: options, ezpwd lookup, all add_test() for tools
├── flake.nix / flake.lock    # the package and the dev shell
├── src/                      # one directory per tool, plus src/library
│   └── efm-decoder/libs/efm/     # EFM library (Reed-Solomon via external ezpwd, see §3)
├── scripts/                  # ld-compress, pcm2wav; test-chroma, test-decode-pretbc (test drivers)
├── test-data/                # pre-generated TBC/EFM/PCM fixtures for functional tests
├── tools/                    # repository scripts run by CI (check-spdx-new-files.sh)
├── docs/                     # per-tool user documentation (Markdown)
└── .github/workflows/        # CI/CD — see §7
```

## 2. Metadata and file formats

- TBC metadata comes in **two fully supported formats**: SQLite (`.tbc.db`, written by current
  ld-decode) and JSON (`.tbc.json`, existing captures and other decoders). Neither is deprecated:
  every reader and writer in `src/library/tbc/` must handle both, and a change to one format
  needs its counterpart in the other.
- The library detects the format from the file contents. `MetadataOptions`
  (`src/library/tbc/metadataoptions.h`) gives the command-line tools `--meta db|json` and the
  checks below; use it rather than building metadata file names by hand.
- **Processing tools never convert between formats**: output metadata is always in the input's
  format, and an output in the other format is refused. Conversion is the job of
  `ld-json-converter` (JSON → SQLite) and `ld-sqlite-to-json` (SQLite → JSON).
- Video-system names: SQLite uses `PAL`, `NTSC`, `PAL_M`; JSON spells the last one `PAL-M`,
  as every JSON reader expects. Readers accept both spellings.
- Any change to the on-disk format (TBC, `.tbc.db`, EFM, PCM output) is a compatibility
  change: call it out in the PR and ask before making it (§10).

## 3. Do not hand-edit vendored or golden files

| Path | Rule |
| --- | --- |
| ezpwd-reed-solomon | External Reed-Solomon headers, never copied into the tree or edited. Nix takes them from a flake input; the packaging workflows clone the same pinned revision into `external/`. Keep the two revisions in step |
| `test-data/**` | Golden fixtures. Do not regenerate or modify them to make a test pass. A change that legitimately alters output updates the **expected values** in `CMakeLists.txt`, with the reason in the PR |

## 4. Coding standards

### 4.1 C++

- C++17 (`CMAKE_CXX_STANDARD 17`), Qt 6. Use Qt types and idioms where the surrounding code
  does (`QString`, `QVector`, `QCommandLineParser`, `qDebug`/`qInfo` via `tbc/logging.h`).
- **There is no formatter gate in this repository.** Match the style of the file you are
  editing, and **do not reformat code you are not otherwise changing** — whitespace-only diffs
  bury the change.
- Keep tools independent: shared logic goes in `src/library/`, never in another tool's
  directory.
- Code must stay portable across Linux (GCC), macOS (Clang) and Windows (MSVC): CI packages
  all three. No POSIX-only headers (`unistd.h`, `getopt`) in tool code; use Qt equivalents.
- Avoid magic numbers in signal-processing code; name constants and cite the standard
  (`// ITU-R BT.1700 ...`, `// SMPTE 170M ...`, `// IEC 60908 ...` for EFM) where one applies.

### 4.2 Licence headers

The project is **GPL-3.0-or-later**. New source files (`.cpp .h .hpp .c .py .sh`) **must**
carry an SPDX header — CI enforces this on added files (`tools/check-spdx-new-files.sh`).
Use the form already in the tree:

```cpp
/******************************************************************************
 * filename.cpp
 * tool-name - one-line purpose
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Contributor Name
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/
```

Existing files without SPDX are converted only when already being edited, never in a sweep.
Do not touch headers of vendored code.

### 4.3 Naming

- Never name a file, symbol, branch or CMake target after a plan step or iteration
  (`phase0`, `v2`, `new`, `final`, `refactored`). Name things after what they do.

## 5. Testing

Source of truth: [TESTING.md](TESTING.md). In short:

| CTest label | What | Needs |
| --- | --- | --- |
| `unit` | `src/library/**/test*` executables | nothing beyond the build |
| `functional` | `scripts/test-chroma`, `scripts/test-decode-pretbc` over `test-data/` | Python 3 + numpy, ffmpeg (all in the dev shell) |

- **Every** `add_test()` must carry exactly one of these labels; CI fails on an unlabelled test.
- Behaviour changes to `src/library/` need a unit test in the matching `test*` directory.
- Do not loosen an expected PSNR / frame count / VBI value to make a test pass without
  explaining why the new value is correct.

## 6. Development environment

**Nix is the supported environment**; CI's Linux gate runs inside the same dev shell.

```bash
nix develop                                   # all dependencies (Qt6, FFTW, ffmpeg, Python, ccache)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L functional --output-on-failure
nix build .#                                  # the package, as Nix users get it
```

One-off without entering the shell: `nix develop -c <command>`. For a temporary tool, use
`nix shell nixpkgs#<tool> -c <tool> ...` — do **not** add it to `flake.nix`.

- In-source builds are rejected; use `build/` or `build-*/`.
- Windows: `build.bat` uses a project-local vcpkg (`vcpkg/`, manifest `vcpkg.json`, Qt under
  the `qt` feature) — see BUILD.md. Keep `vcpkg.json` and `package-windows.yml` in step.
- `EZPWD_DIR` is exported by the dev shell. Outside Nix, pass
  `-DEZPWD_DIR=/path/to/ezpwd-reed-solomon/c++`. `.gitmodules` names ezpwd but **no submodule
  commit is recorded in the tree**, so `git submodule update` fetches nothing; clone
  `pjkundert/ezpwd-reed-solomon` at the revision `flake.lock` pins, as the packaging
  workflows do.

## 7. CI/CD

All in `.github/workflows/`, entered through `main.yml`:

| Workflow | When | What |
| --- | --- | --- |
| `main.yml` | push (all branches, `v*` tags), PR, dispatch | Orchestrates everything below |
| `build-and-test.yml` | always | Static checks (SPDX on new files, script syntax, flake eval); `nix build`; dev-shell build + `unit` and `functional` CTest slices + staged install |
| `package-linux.yml` | push / dispatch | Qt 6.9 build → linuxdeploy → tarball + `ld-analyse` AppImage |
| `package-macos.yml` | push / dispatch | Qt 6.9 build → `macdeployqt` → DMG (CLI tools inside the app bundle) |
| `package-windows.yml` | push / dispatch | MSVC + Qt 6.9 + vcpkg FFTW → `windeployqt` → ZIP |
| `release-from-artifact.yml` | `v*` tags | One GitHub release with every platform's package |

Before proposing build-system or dependency changes, read the platform workflow they affect
and say in the PR what was validated locally and what only CI can show (e.g. Windows/MSVC).
If documentation and a working workflow disagree, the workflow is right — flag the mismatch.

## 8. Licensing

GPL-3.0-or-later. New dependencies must be GPLv3-compatible (GPL, LGPL, BSD, MIT, Apache-2.0,
ISC, …; not AGPL-only, SSPL or proprietary). State the licence of any dependency you add.

## 9. Contribution hygiene

- One problem per PR; no unrelated refactors.
- Update the tool's `README.md` / `docs/` when user-visible behaviour or options change.
- Use the PR template; reference issues.
- Never hard-code secrets or tokens; validate external input (paths, CLI arguments, metadata).

## 10. Ask first when

- a change affects file formats, metadata schema, CLI options or output compatibility;
- several valid approaches have materially different trade-offs;
- a change has licensing, security or performance implications.

## 11. Communication style

Be concise. Prefer bullet lists and fenced commands. Cite files and lines
(`src/library/tbc/sqliteio.cpp:120`) when referring to code.
