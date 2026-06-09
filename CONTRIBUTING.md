# Contributing to OmniSaveSwitch

## Development environment

### Option A — Docker (recommended, no local toolchain needed)

```bash
# Build sysmodule
docker build -t omnisave-sysmodule-builder ./sysmodule
docker run --rm -v "$(pwd)/sysmodule:/src" omnisave-sysmodule-builder make

# Build overlay (fetch libtesla headers first)
mkdir -p overlay/include
LIBTESLA_COMMIT=f766e9b607a05e9756843cbd62b3bfb98be1646c
curl -fsSL "https://raw.githubusercontent.com/WerWolv/libtesla/${LIBTESLA_COMMIT}/include/tesla.hpp" -o overlay/include/tesla.hpp
curl -fsSL "https://raw.githubusercontent.com/WerWolv/libtesla/${LIBTESLA_COMMIT}/include/stb_truetype.h" -o overlay/include/stb_truetype.h
docker build -t omnisave-overlay-builder ./overlay
docker run --rm -v "$(pwd)/overlay:/src" omnisave-overlay-builder make
```

### Option B — Local devkitPro

Install devkitPro with `libnx` and `devkitA64`, then:

```bash
cd sysmodule && make
cd overlay && make
```

## Running tests

```bash
docker compose run --rm test-sysmodule
```

## Deploying to your Switch

```bash
OMNISAVE_SWITCHES=192.168.x.x ./deploy.sh
```

Replace `192.168.x.x` with your Switch's IP. The script copies `exefs.nsp` and `omnisave.ovl` via FTP.

## Commit conventions

This project uses [Conventional Commits](https://www.conventionalcommits.org/). The release automation reads commit messages to determine version bumps:

| Prefix | Version bump |
|---|---|
| `fix:` | patch (1.0.x) |
| `feat:` | minor (1.x.0) |
| `feat!:` or `BREAKING CHANGE:` | major (x.0.0) |
| `chore:`, `docs:`, `refactor:` | no bump |

Examples:
```
feat: add auto-backup on game close
fix: prevent double-upload when Switch wakes from sleep
feat!: change save format to ZIP/STORE — requires re-pairing server
```

## Issue-first workflow

Open an issue before starting work on a non-trivial feature or fix. This avoids duplicate effort and lets maintainers flag design constraints early.

## Pull request guidelines

- Keep PRs under 500 lines of code changes. Large refactors should be split.
- Every PR must pass CI (sysmodule build, overlay build, tests).
- If your change touches file I/O, transaction handling, or save assembly, include a safety proof in the PR description explaining why save data cannot be corrupted.
- Use the PR template checklist — do not delete items you have not verified.

## P0 safety rule

Any change that has a non-zero chance of corrupting or deleting save data requires an explicit safety proof in the PR description. This is mandatory, not optional. If you cannot write a convincing safety proof, the change should not be merged.

## Release flow

Releases are fully automated via release-please. You do not need to manually bump versions or tag commits. Merging a conventional-commit PR to `main` is sufficient — release-please handles the rest.
