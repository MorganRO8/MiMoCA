# MiMoCA Release Readiness Checklist

Use this checklist before creating a production release build.

## Automated readiness (must pass)

- [ ] Fresh install succeeds from packaged layout on a clean Windows profile.
- [ ] Sidecar auto-start succeeds from launcher (`scripts/launch_mimoca.bat`).
- [ ] Model prefetch is complete for required modalities (STT + vision; gesture when enabled).
- [ ] Build/package smoke validation script passes and writes `artifacts/readiness/smoke_report.json`.

## Manual readiness (must pass)

- [ ] API key prompt appears in `llm` mode and key is securely saved (Windows Credential Manager + DPAPI, not plaintext).
- [ ] Live microphone conversation works and user speech interrupts TTS playback.
- [ ] Camera preview is visible and gesture branch selection (`option_a` / `option_b`) updates recipe flow.
- [ ] Manual QA scenario script is executed and writes `artifacts/readiness/manual_qa_report.json`.

## Release gate

- [ ] `python3 scripts/release_gate.py --checklist scripts/release_checklist.md --smoke-report artifacts/readiness/smoke_report.json --manual-report artifacts/readiness/manual_qa_report.json` returns success.
- [ ] Release build target (`cmake --build <build_dir> --config Release --target mimoca`) passes readiness gate.
