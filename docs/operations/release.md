# Release Runbook

This runbook defines the release contract for `xv6-esp`.

## Versioning

- Use semantic tags: `vMAJOR.MINOR.PATCH` (example: `v1.4.2`).
- A release is valid only when CI gates pass for the tagged commit.

## Preconditions

1. `CI Gates` workflow is green.
2. `Static Analysis` workflow is green.
3. `ESP32 QEMU` workflow is green.
4. `RISC-V CI` workflow is green.
5. (Recommended) `ESP32 HIL Gate` completed on hardware for the release candidate commit.

## Release procedure

1. Create and push the release tag:
```bash
git tag vX.Y.Z
git push origin vX.Y.Z
```
2. GitHub Action `.github/workflows/release.yml` runs automatically on `v*`.
3. Workflow executes:
   - ESP build
   - CI gate sequence (`scripts/ci_gate_esp.py`)
   - release bundling (`scripts/release_bundle.py`)
   - GitHub Release publication with artifacts

## Artifacts

Release workflow publishes:

- `dist/releases/<tag>/manifest.json`
- `dist/releases/<tag>/SHA256SUMS`
- `dist/releases/<tag>/xv6-esp-<tag>.tar.gz`
- `dist/releases/<tag>/files/*`

Minimum required binaries in `files/`:

- `bootloader.bin`
- `partition-table.bin`
- `ota_data_initial.bin`
- `xv6_esp32s3.bin`
- `xv6fs.bin`

## Local dry-run

Before tagging, validate packaging locally:

```bash
source "$HOME/esp-idf/export.sh"
idf.py set-target esp32s3
python3 ./scripts/ci_gate_esp.py
python3 ./scripts/release_bundle.py --version vX.Y.Z --commit "$(git rev-parse HEAD)"
```

## Rollback

1. Select the previous stable tag release.
2. Verify `SHA256SUMS` for target binaries.
3. Flash previous known-good firmware image.
4. Record incident in `docs/operations/incident-response.md` timeline.
