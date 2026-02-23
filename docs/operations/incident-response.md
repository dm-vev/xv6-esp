# Incident Response Runbook

This runbook is for production failures on ESP hardware and QEMU gates.

## Severity model

- `SEV-1`: boot loop, persistent crash, filesystem corruption, or inability to flash/boot.
- `SEV-2`: degraded shell/runtime behavior with workaround.
- `SEV-3`: non-blocking regression caught in CI/HIL without user impact.

## First response (first 15 minutes)

1. Capture build identity:
   - git commit
   - release tag (if any)
   - target board and serial port
2. Collect logs:
   - CI artifacts in `build/triage/*` (if CI failure)
   - serial monitor output (if HIL/device failure)
3. Run shell diagnostic command:
   - `health`
4. Record panic markers if present:
   - `Guru Meditation Error`
   - `Backtrace:`
   - `assert failed:`
   - watchdog messages

## Triage commands

ESP/QEMU local triage:

```bash
source "$HOME/esp-idf/export.sh"
python3 ./scripts/ci_gate_esp.py
python3 ./scripts/qemu_smoke_esp.py
python3 ./scripts/qemu_regressions_esp.py
```

RISC-V baseline triage:

```bash
make kernel/kernel fs.img
python3 ./test-xv6.py -q usertests
python3 ./test-xv6.py crash
```

Hardware triage:

```bash
source "$HOME/esp-idf/export.sh"
python3 ./scripts/hil_smoke_esp.py --port /dev/ttyACM0 --flash
python3 ./scripts/hil_gate_esp.py --port /dev/ttyACM0 --cycles 3 --flash-first
```

## Containment

1. Stop rollout of the failing revision.
2. Revert to last known-good release artifact.
3. Keep a single owner for incident command and timeline updates.

## Recovery and verification

1. Patch and rebuild.
2. Re-run full CI gate sequence.
3. Re-run HIL smoke/gate on target hardware.
4. Verify `health` output is stable (uptime progressing, heap non-zero, no runaway jobs).

## Post-incident

1. Document root cause and trigger.
2. Add regression coverage (QEMU/HIL/RISC-V) for the failure mode.
3. Link the fix commit, test evidence, and release tag.
