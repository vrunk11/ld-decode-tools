# Contributing

Thanks for helping keep ld-decode-tools working.

## Before you start

- Search existing issues and pull requests to avoid duplicates.
- For anything that changes a file format, the metadata schema or a tool's command-line
  options, open an issue first.

## Workflow

1. Fork and create a branch named after what it does (not `fix2`, `new`, `final`).
2. Build and test in the Nix dev shell — see [BUILD.md](BUILD.md) and [TESTING.md](TESTING.md).
3. Keep the change focused; do not reformat code you are not otherwise changing.
4. New source files carry an SPDX header (see [AGENTS.md](AGENTS.md) §4.2).
5. Open a pull request using the template and say what you validated and on which platforms.

CI builds and tests on Linux through Nix for every push and pull request, and packages for
Linux, macOS and Windows on every push to a branch of this repository.

## Commit messages

Describe the change and why, reference issues where relevant. Do not add tool or AI
attribution trailers (see [AGENTS.md](AGENTS.md), Rule 2).

## Licence

By contributing you agree that your contribution is licensed under the
[GNU GPL v3 or later](LICENSE).
