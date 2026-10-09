## Summary

Briefly describe what this PR changes and why.

> ld-decode-tools is deprecated in favour of [decode-orc](https://github.com/simoninns/decode-orc).
> Fixes, compatibility and build/CI maintenance are welcome here; new features belong there.

## Component

- [ ] `ld-analyse` (GUI)
- [ ] `ld-chroma-decoder` / `ld-chroma-encoder`
- [ ] VBI / VITS / dropout / stacker / discmap tools
- [ ] Metadata export / conversion (`ld-export-*`, `ld-json-converter`, `ld-lds-converter`)
- [ ] EFM decoder suite (`src/efm-decoder`)
- [ ] Shared library (`src/library`)
- [ ] Build, CI, packaging or Nix flake
- [ ] Documentation

## Type of change

- [ ] Bug fix
- [ ] Compatibility / portability
- [ ] Refactor
- [ ] Documentation
- [ ] Build / CI
- [ ] Other

## Validation

Describe how you tested this change — the commands you ran and what they reported.

- [ ] `ctest --test-dir build -L unit --output-on-failure`
- [ ] `ctest --test-dir build -L functional --output-on-failure`
- [ ] `nix build .#`
- [ ] Platforms not validated locally (e.g. Windows/MSVC, macOS) are listed here:

## Checklist

- [ ] Scope is focused and minimal
- [ ] New source files carry an SPDX header (`tools/check-spdx-new-files.sh`)
- [ ] File-format / metadata-schema / CLI-option changes are called out above
- [ ] Expected test values were not changed, or the change is justified above
- [ ] Tool README / `docs/` updated if user-visible behaviour changed
- [ ] Follows [AGENTS.md](../AGENTS.md)

## Related issue

Closes #
