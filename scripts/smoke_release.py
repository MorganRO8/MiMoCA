#!/usr/bin/env python3
"""Lightweight non-interactive smoke checks for MiMoCA release artifacts."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
from dataclasses import dataclass


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str


def _run(cmd: list[str], cwd: pathlib.Path) -> tuple[bool, str]:
    proc = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True, check=False)
    combined = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    return proc.returncode == 0, combined.strip()


def _python_compile(repo: pathlib.Path) -> CheckResult:
    files = ["python/service.py", "python/bootstrap_sidecar_env.py"]
    ok, out = _run([sys.executable, "-m", "py_compile", *files], repo)
    return CheckResult("python_compile", ok, out or "python files compiled")


def _cmake_configure(repo: pathlib.Path, build_dir: pathlib.Path) -> CheckResult:
    cmd = ["cmake", "-S", ".", "-B", str(build_dir)]
    if os.name == "nt":
        # Keep smoke builds aligned with the supported Windows-first toolchain.
        cmd.extend(["-G", "Visual Studio 17 2022", "-T", "v143", "-A", "x64", "-DCMAKE_SYSTEM_VERSION=10.0"])
    ok, out = _run(cmd, repo)
    return CheckResult("cmake_configure", ok, out)


def _cmake_build(repo: pathlib.Path, build_dir: pathlib.Path, config: str) -> CheckResult:
    cmd = ["cmake", "--build", str(build_dir)]
    if os.name == "nt":
        cmd.extend(["--config", config])
    ok, out = _run(cmd, repo)
    return CheckResult("cmake_build", ok, out)


def _cmake_install(repo: pathlib.Path, build_dir: pathlib.Path, stage_dir: pathlib.Path, config: str) -> CheckResult:
    cmd = ["cmake", "--install", str(build_dir), "--prefix", str(stage_dir)]
    if os.name == "nt":
        cmd.extend(["--config", config])
    ok, out = _run(cmd, repo)
    return CheckResult("cmake_install", ok, out)


def _staged_layout(stage_dir: pathlib.Path) -> CheckResult:
    root = stage_dir / "MiMoCA"
    bin_name = "mimoca.exe" if os.name == "nt" else "mimoca"
    expected = [
        root / "bin" / bin_name,
        root / "assets" / "recipes.json",
        root / "python" / "service.py",
        root / "scripts" / "launch_mimoca.bat",
        root / "scripts" / "first_launch_setup.ps1",
    ]
    missing = [str(p) for p in expected if not p.exists()]
    if missing:
        return CheckResult("staged_layout", False, "Missing files: " + ", ".join(missing))
    return CheckResult("staged_layout", True, f"Validated {len(expected)} required files")


def _installer_points_to_release(repo: pathlib.Path) -> CheckResult:
    iss_path = repo / "installer" / "mimoca.iss"
    if not iss_path.exists():
        return CheckResult("installer_script", False, f"Missing file: {iss_path}")
    text = iss_path.read_text(encoding="utf-8")
    needle = 'Source: "release\\MiMoCA\\*"'
    ok = needle in text
    detail = "installer consumes release layout" if ok else f"Expected token not found: {needle}"
    return CheckResult("installer_script", ok, detail)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run MiMoCA release smoke checks")
    parser.add_argument("--repo", default=".", help="Repository root")
    parser.add_argument("--build-dir", default="build_smoke", help="Temporary build directory")
    parser.add_argument("--stage-dir", default="release_smoke", help="Temporary install staging directory")
    parser.add_argument("--config", default="Release", help="Build configuration")
    parser.add_argument(
        "--report",
        default="artifacts/readiness/smoke_report.json",
        help="JSON report output path",
    )
    args = parser.parse_args()

    repo = pathlib.Path(args.repo).resolve()
    build_dir = repo / args.build_dir
    stage_dir = repo / args.stage_dir
    report_path = (repo / args.report).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)

    if build_dir.exists():
        shutil.rmtree(build_dir)
    if stage_dir.exists():
        shutil.rmtree(stage_dir)

    checks: list[CheckResult] = []
    checks.append(_python_compile(repo))
    if checks[-1].ok:
        checks.append(_cmake_configure(repo, build_dir))
    if checks[-1].ok:
        checks.append(_cmake_build(repo, build_dir, args.config))
    if checks[-1].ok:
        checks.append(_cmake_install(repo, build_dir, stage_dir, args.config))
    if checks[-1].ok:
        checks.append(_staged_layout(stage_dir))
    checks.append(_installer_points_to_release(repo))

    overall_ok = all(item.ok for item in checks)
    report = {
        "ok": overall_ok,
        "generated_at": pathlib.Path(__file__).name,
        "checks": [{"name": c.name, "ok": c.ok, "detail": c.detail} for c in checks],
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for item in checks:
        status = "PASS" if item.ok else "FAIL"
        print(f"[{status}] {item.name}: {item.detail}")
    print(f"Smoke report: {report_path}")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
