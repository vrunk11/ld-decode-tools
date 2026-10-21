# Testing

How ld-decode-tools is tested, locally and in CI.

## Test slices

Every test registered with CTest carries exactly one label. CI runs each label as its own
slice and fails if a test carries neither.

| Label | Tests | Where defined | Needs |
| --- | --- | --- | --- |
| `unit` | the library's (`testfilter`, `testaudio`, `testcvbs`, `testldx`, `testlinenumber`, `testmetadata`, `testtbcfiles`, `testvbidecoder`, `testvitcdecoder`, `testdiscnumbering`, `testdropouts`, `testvitsmetrics`, `testsqliteschema`, `teststackframe`, `teststackercore`, `testcommonoptions`, `testpipedinputoptions`, `testsourcefilechecks`, `testfieldpool`), the chroma decoder's (`testlumacoring`), the windows' (`testdropoutdrawing`, `testaspectratio`, `testappearancemenus` in `gui-common`; `testremote`, `testshortcuts`, `testvideofilter`, `testwaveform`, `testweave` in `ld-analyse`; `teststackproject` in `ld-stacker-gui`) and `ld-discmap`'s (`testframe`) | `src/**/test*/CMakeLists.txt` | the build only (the tests of the windows run without a screen) |
| `functional` | `chroma-*`, `decode-pretbc-*` | root `CMakeLists.txt` | Python 3 + numpy, ffmpeg |

### Unit tests

Small executables that exercise `src/library` (filters, line numbering, metadata, VBI and
VITC decoding) with in-code inputs. A behaviour change in `src/library/` should come with a
case in the matching `test*` directory. New unit tests:

```cmake
add_test(NAME testfoo COMMAND testfoo)
set_tests_properties(testfoo PROPERTIES LABELS unit)
```

A test that checks with `assert()` must be built with `-UNDEBUG` (`target_compile_options(testfoo PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)`): the Release and
RelWithDebInfo builds define `NDEBUG`, which removes every `assert()` and leaves a test that checks nothing. Or check with a macro that does not depend on it, as `testdiscnumbering` does.

### Functional tests

End-to-end runs of the built tools:

- **`scripts/test-chroma`** generates a test pattern with ffmpeg, encodes it with
  `ld-chroma-encoder`, decodes it with `ld-chroma-decoder`, and checks the PSNR against the
  original. The four chroma tests are chained with `DEPENDS` because they share output files,
  so this slice is run without `--parallel`.
- **`scripts/test-decode-pretbc`** runs the processing chain (`ld-process-vbi`,
  `ld-dropout-correct`, `ld-chroma-decoder`, EFM decoding, …) over the pre-generated TBCs in
  `test-data/` and checks frame counts, black PSNR, VBI and VITC values and EFM sample counts.

`test-data/` is golden. Do not regenerate it to make a test pass; if a change legitimately
alters output, update the expected value in `CMakeLists.txt` and justify it in the PR.

## Running locally

```bash
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L functional --output-on-failure

# a single test
ctest --test-dir build -R decode-pretbc-pal-cav --output-on-failure

# the Nix package, exactly as users receive it
nix build .#
```

If a chroma test fails, rerun the script by hand with `--png` and look at the images
(`scripts/test-chroma --help`).

## In CI

`.github/workflows/build-and-test.yml` runs on every push and pull request:

| Job | Checks |
| --- | --- |
| Static Checks | SPDX header on files added by the change, Python script syntax, `nix flake check --no-build` |
| Nix Package Build | `nix build .#`, then `--help` on a sample of the packaged tools |
| Unit + Functional Tests | dev-shell build with ccache, label presence, `unit` slice, `functional` slice, unlabelled-test check, staged `cmake --install` |

JUnit results (`ctest-results-*.xml`) and `LastTest.log` are uploaded as the
`ctest-artifacts` artifact on every run, including failed ones.

The packaging workflows (Linux, macOS, Windows) build without tests and only smoke-test that
each bundled tool starts and answers `--help`.
