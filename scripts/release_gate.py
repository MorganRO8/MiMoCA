#!/usr/bin/env python3
"""Release readiness gate for MiMoCA."""

from __future__ import annotations

import argparse
import json
import pathlib
import re

REQUIRED_CHECKLIST_LINES = [
    "Fresh install succeeds",
    "Sidecar auto-start succeeds",
    "Model prefetch is complete",
    "API key prompt appears",
    "Live microphone conversation works and user speech interrupts TTS",
    "Camera preview is visible and gesture branch selection",
]

REQUIRED_SMOKE_CHECKS = {
    "python_compile",
    "cmake_configure",
    "cmake_build",
    "cmake_install",
    "staged_layout",
    "installer_script",
}


def _validate_checklist(path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not path.exists():
        return [f"checklist missing: {path}"]
    text = path.read_text(encoding="utf-8")
    for label in REQUIRED_CHECKLIST_LINES:
        pattern = re.compile(rf"^- \[x\] .*{re.escape(label)}", re.IGNORECASE | re.MULTILINE)
        if not pattern.search(text):
            errors.append(f"checklist item not marked complete: {label}")
    return errors


def _validate_smoke_report(path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not path.exists():
        return [f"smoke report missing: {path}"]
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("ok") is not True:
        errors.append("smoke report overall status is not ok=true")
    checks = {item.get("name"): item for item in data.get("checks", [])}
    for required in REQUIRED_SMOKE_CHECKS:
        entry = checks.get(required)
        if not entry:
            errors.append(f"smoke check missing: {required}")
            continue
        if entry.get("ok") is not True:
            errors.append(f"smoke check failed: {required}")
    return errors


def _validate_manual_report(path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not path.exists():
        return [f"manual QA report missing: {path}"]
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("ok") is not True:
        errors.append("manual QA report overall status is not ok=true")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate release readiness artifacts")
    parser.add_argument("--checklist", required=True)
    parser.add_argument("--smoke-report", required=True)
    parser.add_argument("--manual-report", required=True)
    args = parser.parse_args()

    checklist = pathlib.Path(args.checklist).resolve()
    smoke = pathlib.Path(args.smoke_report).resolve()
    manual = pathlib.Path(args.manual_report).resolve()

    errors = []
    errors.extend(_validate_checklist(checklist))
    errors.extend(_validate_smoke_report(smoke))
    errors.extend(_validate_manual_report(manual))

    if errors:
        for err in errors:
            print(f"[FAIL] {err}")
        return 1

    print("[PASS] Release readiness gate passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
