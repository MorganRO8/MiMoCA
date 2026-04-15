#!/usr/bin/env python3
"""Create/update the managed MiMoCA sidecar virtual environment."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import venv


def _interpreter_path(venv_path: pathlib.Path) -> pathlib.Path:
    if os.name == "nt":
        return venv_path / "Scripts" / "python.exe"
    return venv_path / "bin" / "python3"


def main() -> int:
    parser = argparse.ArgumentParser(description="Bootstrap MiMoCA sidecar Python environment")
    parser.add_argument("--venv-path", required=True, help="Virtual environment directory")
    parser.add_argument("--requirements", required=True, help="Path to requirements.txt")
    args = parser.parse_args()

    venv_path = pathlib.Path(args.venv_path).resolve()
    requirements = pathlib.Path(args.requirements).resolve()

    try:
        if not requirements.exists():
            raise FileNotFoundError(f"requirements file not found: {requirements}")

        venv_path.parent.mkdir(parents=True, exist_ok=True)
        builder = venv.EnvBuilder(with_pip=True, clear=False, upgrade=False)
        builder.create(str(venv_path))

        interpreter = _interpreter_path(venv_path)
        if not interpreter.exists():
            raise FileNotFoundError(f"venv interpreter missing: {interpreter}")

        install = subprocess.run(
            [str(interpreter), "-m", "pip", "install", "-r", str(requirements)],
            text=True,
            capture_output=True,
            check=False,
        )
        if install.returncode != 0:
            detail = (install.stderr or install.stdout).strip()
            raise RuntimeError(f"pip install failed: {detail}")

        vision_check = subprocess.run(
            [
                str(interpreter),
                "-c",
                (
                    "import importlib.util\n"
                    "missing=[]\n"
                    "missing += ['ultralytics'] if importlib.util.find_spec('ultralytics') is None else []\n"
                    "missing += ['clip (OpenAI CLIP)'] if importlib.util.find_spec('clip') is None else []\n"
                    "print(','.join(missing))\n"
                    "raise SystemExit(1 if missing else 0)\n"
                ),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if vision_check.returncode != 0:
            detail = (vision_check.stdout or vision_check.stderr).strip()
            raise RuntimeError(
                "vision dependency verification failed. "
                "Re-run bootstrap so requirements install ultralytics + OpenAI CLIP. "
                f"Missing: {detail or 'unknown'}"
            )

        print(
            json.dumps(
                {
                    "ok": True,
                    "venv_path": str(venv_path),
                    "interpreter_path": str(interpreter),
                }
            )
        )
        return 0
    except Exception as exc:  # keep bootstrap output actionable
        print(
            json.dumps(
                {
                    "ok": False,
                    "venv_path": str(venv_path),
                    "interpreter_path": str(_interpreter_path(venv_path)),
                    "error": str(exc),
                }
            )
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
