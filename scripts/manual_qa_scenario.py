#!/usr/bin/env python3
"""Manual QA checklist runner for full voice/gesture release scenario."""

from __future__ import annotations

import argparse
import json
import pathlib
from datetime import datetime, timezone

REQUIRED_STEPS = [
    "fresh_install_succeeds",
    "sidecar_auto_start",
    "model_prefetch_complete",
    "api_key_prompt_and_secure_save",
    "live_mic_conversation_with_tts_interrupt",
    "camera_preview_and_gesture_branch_selection",
]


def _blank_report(tester: str) -> dict:
    return {
        "ok": False,
        "tester": tester,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "steps": [
            {"id": step, "passed": False, "notes": ""}
            for step in REQUIRED_STEPS
        ],
    }


def _validate(report: dict) -> tuple[bool, list[str]]:
    errors: list[str] = []
    steps = {step.get("id"): step for step in report.get("steps", [])}
    for required in REQUIRED_STEPS:
        if required not in steps:
            errors.append(f"missing step: {required}")
            continue
        if steps[required].get("passed") is not True:
            errors.append(f"step not passed: {required}")
    return len(errors) == 0, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate or validate manual QA report")
    parser.add_argument("--report", default="artifacts/readiness/manual_qa_report.json")
    parser.add_argument("--init", action="store_true", help="Create a blank report template")
    parser.add_argument("--tester", default="", help="Tester name for --init")
    parser.add_argument("--validate", action="store_true", help="Validate an existing report")
    args = parser.parse_args()

    report_path = pathlib.Path(args.report).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)

    if args.init:
        report_path.write_text(json.dumps(_blank_report(args.tester), indent=2), encoding="utf-8")
        print(f"Template written: {report_path}")
        return 0

    if args.validate:
        if not report_path.exists():
            print(f"Report missing: {report_path}")
            return 1
        data = json.loads(report_path.read_text(encoding="utf-8"))
        ok, errors = _validate(data)
        data["ok"] = ok
        report_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
        if ok:
            print("Manual QA report valid.")
            return 0
        print("Manual QA report invalid:")
        for err in errors:
            print(f"- {err}")
        return 1

    parser.error("Specify --init or --validate")


if __name__ == "__main__":
    raise SystemExit(main())
