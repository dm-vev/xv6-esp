# Configuration and Secrets Policy

This project uses `sdkconfig.defaults` as the single committed baseline for ESP-IDF configuration.
Generated `sdkconfig` files are machine-specific build artifacts and must not be used as a secret store.

## Local configuration overlays

Use a local override defaults file for environment-specific settings:

```bash
cat > sdkconfig.local.defaults <<'EOF'
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
EOF
```

Build with baseline + local overlay:

```bash
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local.defaults" set-target esp32s3
idf.py build
```

Do not commit `sdkconfig.local.defaults`, `sdkconfig.local`, `.env`, or other machine-local files.

## Secrets handling

- Never place credentials, tokens, Wi-Fi passwords, API keys, or serial endpoints in tracked files.
- Use CI secret storage (`Settings -> Secrets and variables -> Actions`) for sensitive data.
- For HIL automation, pass runtime values through workflow inputs or repository/environment variables:
  - `HIL_PORT`
  - `HIL_BAUD`
  - `HIL_CYCLES`
- Treat serial device names as operational parameters, not hardcoded source config.

## Required CI defaults

All CI jobs must build from committed defaults and must not require interactive `menuconfig`.
For strict reproducibility, CI jobs should call:

```bash
idf.py set-target esp32s3
idf.py build
```

and fail if required tools are missing (`scripts/static_analysis.sh` defaults to `STRICT=1`).
