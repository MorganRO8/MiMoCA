# README.md

# Multimodal Cooking Assistant

A **Windows-first multimodal cooking assistant prototype** built around a **C++ desktop app** with a **minimal Python sidecar**.

## Current vertical slice

This repo now includes:
- C++ shell app (`src/main.cpp`)
- Python sidecar (`python/service.py`) with:
  - `GET /health`
  - `POST /plan` mock planner using the canonical `TurnContext` and `PlannerResponse` contracts
  - `POST /vision/detect` Ultralytics YOLO object detection with a fixed cooking vocabulary
- Recipe asset file (`assets/recipes.json`) with one real branching demo (`rice_cooker` vs `pot`)

The C++ app now runs a tiny end-to-end path:
1. load one recipe from `assets/recipes.json`
2. track current step and selected recipe branches in memory
3. health-check sidecar
4. accept utterances through the Python STT boundary by sending buffered WAV audio (`stt-file <path.wav>`) or shortcut commands (`current`, `next`)
5. run MediaPipe hand tracking on the latest camera frame and classify a tiny fixed gesture vocabulary
6. send turn context to `POST /plan` including `user_utterance` plus `gesture.label` and `gesture.confidence`
7. transcribe audio in the sidecar with `faster-whisper` (CTranslate2 backend) and return transcript text to the app
8. parse `PlannerResponse`
9. speak `assistant_text` with Windows SAPI when `speak` is true
10. map gesture predictions into app labels (`next`, `repeat`, `option_a`, `option_b`, `none`) and send them in `TurnContext`
11. select and persist branch choices from utterance (`rice cooker` / `pot`) or gestures (`option_a` / `option_b`)
12. advance local step if `advance_step` is true, using branch-specific next-step routing when a branch is selected
13. start camera capture on device `0` and keep a latest-frame summary (availability, width/height, timestamp, frame count) for planner turns
14. optionally emit compact debug snapshots (transcript, gesture, detections, recipe/step/branch, planner round-trip status)
15. run Ultralytics YOLO in the sidecar to populate `TurnContext.detections` from the latest camera frame (`label`, `confidence`, `bbox`)

Both C++ and Python log serialized planner request/response JSON at the service boundary.
The C++ app also logs camera lifecycle events (start/stop/first-frame availability).

## Build and run (minimal)

### 1) Build C++ app

```bash
cmake -S . -B build
cmake --build build
```

### 2) Bootstrap managed sidecar environment (automatic)

On installer-first-run and normal first app run, MiMoCA now bootstraps a local sidecar virtual environment automatically:
- creates `.mimoca_sidecar_venv`
- installs `python/requirements.txt`
- persists app defaults in `mimoca_app_config.json` (`sidecar_env_path`, `planner_mode`)
- launches `python/service.py` with that exact venv interpreter

You can also run the same bootstrap tool manually (for installer integration or repair flows):

```bash
python3 python/bootstrap_sidecar_env.py \
  --venv-path .mimoca_sidecar_venv \
  --requirements python/requirements.txt
```

### 3) Start Python sidecar manually (developer mode)

From repo root:

```bash
python3 python/service.py
```

The sidecar listens on `http://127.0.0.1:8080`.

Startup now performs explicit modality initialization and readiness orchestration:
- verifies/downloads Faster-Whisper model (`MIMOCA_STT_MODEL`)
- verifies/downloads YOLO model (`MIMOCA_VISION_MODEL`)
- verifies/downloads MediaPipe hand model (`MIMOCA_GESTURE_MODEL_PATH`, optionally fetched from `MIMOCA_GESTURE_MODEL_URL`)
- reports per-modality readiness via `/health` with statuses (`downloading`, `ready`, `failed`) and progress
- blocks sidecar "ready" until required modalities are initialized, unless degraded mode is allowed
- exposes planner runtime configuration endpoints:
  - `POST /planner/validate_key` validates a candidate API key against the provider (`GET /models`)
  - `POST /planner/configure` updates planner mode/key without restarting the sidecar

Vision defaults:
- `MIMOCA_VISION_MODEL=yolov8s-worldv2.pt`
- `MIMOCA_VISION_CONFIDENCE=0.25`
- fixed vocabulary: `onion`, `knife`, `cutting board`, `pot`, `pan`, `bowl`, `spoon`, `rice cooker`

For MediaPipe gesture detection, provide a Hand Landmarker task model at:

```bash
python/models/hand_landmarker.task
```

Or override with:

```bash
MIMOCA_GESTURE_MODEL_PATH=/full/path/to/hand_landmarker.task python3 python/service.py
```

Model cache paths are configurable:
- `MIMOCA_MODEL_CACHE_ROOT` (default `python/model_cache`)
- `MIMOCA_STT_CACHE_ROOT`
- `MIMOCA_VISION_CACHE_ROOT`
- `MIMOCA_GESTURE_CACHE_ROOT`
- `MIMOCA_REQUIRED_MODALITIES` (default `stt,vision`)
- `MIMOCA_ALLOW_DEGRADED_STARTUP` (`true` by default)

### 4) Run C++ app

```bash
./build/mimoca
```

On Windows builds with Qt6 available, startup opens the primary UI directly:
- large camera preview area
- right-side transcript/chat history
- status indicators for mic, gesture, planner, and sidecar health
- developer-only debug toggle panel
- planner settings button for API key update/revoke

If planner mode is `llm` and no valid key is available at startup, the UI prompts for an OpenAI API key, validates it, and stores it in Windows Credential Manager + DPAPI (not plaintext config). Optional “skip for now (mock mode)” is shown only when `MIMOCA_ALLOW_PLANNER_SKIP_TO_MOCK=true`.

Manual debug/console commands are no longer required for primary operation in that mode.
On non-Qt builds, the executable prints a fallback message and exits.

Legacy command list (for older non-UI shell flow):
- `current` → ask planner for current step
- `next` → ask planner for next instruction (and advance if available)
- `gesture` → run a gesture-only planner turn using the latest camera frame and sidecar gesture detection
- `stt-file ./path/to/audio.wav` → transcribe a buffered WAV clip using faster-whisper and run a planner turn
- `stt-stream-file ./path/to/audio.wav` → stream WAV PCM to sidecar in ~20 ms chunks, use VAD speech-start to auto-stop TTS, and segment utterances on silence
- `camera` → print camera status and latest frame summary
- `stop` → cancel current TTS playback (temporary manual interruption control)
- `debug on` / `debug off` / `debug status` → enable, disable, or print compact debug state
- `exit` → quit

To choose a different model/device at startup:

```bash
MIMOCA_STT_MODEL=large-v3 MIMOCA_STT_DEVICE=cpu python3 python/service.py
```

To choose a Windows TTS voice (if installed and available):

```bash
MIMOCA_TTS_VOICE=Zira ./build/mimoca
```

`MIMOCA_TTS_VOICE` is matched against installed SAPI voice descriptions/token IDs. If no match is found, the app logs a fallback and uses the default system voice.

Defaults:
- `MIMOCA_STT_MODEL=distil-large-v3` (auto-fallback to `large-v3` if unavailable)
- `MIMOCA_STT_DEVICE=cpu`
- `MIMOCA_STT_COMPUTE_TYPE=int8`

If OpenCV is available at build time, camera capture is enabled automatically. If OpenCV is not found, the app stays in graceful camera-disabled mode and still runs speech + planner flow.
Debug mode is disabled by default and can also be enabled at startup with `MIMOCA_DEBUG=1 ./build/mimoca`.

## Manual health-path testing

### Health check

```bash
curl http://127.0.0.1:8080/health
```

Expected: JSON with `status: "ok"`.
When startup initialization is still running, `/health` returns `503` with
`status: "initializing"` and includes startup progress/status in `startup`.

### STT path (buffered WAV)

```bash
python3 - <<'PY'
import base64, json, pathlib, urllib.request
wav = pathlib.Path("sample.wav").read_bytes()
payload = json.dumps({
    "audio_base64": base64.b64encode(wav).decode("utf-8"),
    "audio_format": "wav",
    "is_final": True
}).encode("utf-8")
req = urllib.request.Request(
    "http://127.0.0.1:8080/stt/transcribe",
    data=payload,
    headers={"Content-Type": "application/json"},
    method="POST",
)
print(urllib.request.urlopen(req).read().decode("utf-8"))
PY
```

Expected: JSON containing `text` and `is_final`.

### Planner path with recipe step context

```bash
curl -X POST http://127.0.0.1:8080/plan \
  -H "Content-Type: application/json" \
  -d '{
    "timestamp": "2026-01-01T00:00:00Z",
    "recipe_id": "demo_omelet",
    "step_id": "step_1",
    "branch_id": null,
    "user_utterance": "what next?",
    "current_step_instruction": "Crack two eggs into a bowl and whisk with a pinch of salt.",
    "next_step_instruction": "Heat butter in a pan over medium heat and pour in the eggs.",
    "gesture": {"label": "next", "confidence": 0.98},
    "detections": [{"label": "knife", "confidence": 0.93, "bbox": [0.1, 0.2, 0.3, 0.4]}],
    "hand_pose": {"label": "safe_claw", "confidence": 0.87},
    "settings": {
      "speech_enabled": true,
      "vision_enabled": true,
      "gesture_enabled": true,
      "tts_enabled": true
    }
  }'
```

Expected: deterministic mock `PlannerResponse` JSON payload that can request step advancement.

## Notes

- STT is now a real sidecar adapter backed by `faster-whisper` with CTranslate2, CPU-first by default.
- The sidecar exposes both buffered transcription (`/stt/transcribe`) and chunk-buffering endpoints (`/stt/session/start`, `/stt/session/chunk`, `/stt/session/finalize`) so partial/final transcript behavior can be added without redesigning the API.
- The sidecar now includes lightweight RMS-based VAD in the chunk pipeline. When speech start is detected, the C++ app immediately interrupts TTS in streamed mode.
- VAD segments utterances by detecting trailing silence. If VAD becomes unavailable (for example unsupported sample rate), interruption falls back to the manual `stop` command.
- The sidecar now exposes `/gesture/detect`, backed by MediaPipe Hand Landmarker with a tiny heuristic classifier for `next`, `repeat`, `option_a`, `option_b`, `none`.
- The sidecar now exposes `/vision/detect`, backed by Ultralytics YOLO and constrained to a small fixed cooking vocabulary. The C++ app sends the latest camera frame, receives detections (`label`, `confidence`, `bbox`), and forwards them in `TurnContext`.
- The planner now supports two modes: `mock` (deterministic existing behavior) and `llm` (real provider-backed planner with structured JSON output and mock fallback).
- If the YOLO model path points to an ONNX model, Ultralytics can run it with ONNX Runtime (`onnxruntime` dependency included).
- The sidecar boundary is intentionally small and logged to keep iteration fast.
- Recipe parsing is intentionally minimal and currently targets one startup recipe.

## Installer notes (managed Python)

- The installer should invoke `python/bootstrap_sidecar_env.py` during install or handoff to first-run bootstrap in the app.
- If bootstrap fails, the app shows an actionable error with **Retry** so users can fix Python/network issues and continue.
- The app always prefers `MIMOCA_PYTHON_EXECUTABLE` when explicitly set; otherwise it uses the persisted managed environment interpreter.

## Real LLM planner mode

By default, planner mode is `llm` (production default):

```bash
python3 python/service.py
```

For explicit local mock mode (development/testing only):

```bash
MIMOCA_PLANNER_MODE=mock python3 python/service.py
```

Enable/configure LLM planning (OpenAI-compatible HTTP API):

```bash
MIMOCA_PLANNER_MODE=llm \
MIMOCA_LLM_PROVIDER=openai_compatible \
MIMOCA_LLM_BASE_URL=https://api.openai.com/v1 \
MIMOCA_LLM_MODEL=gpt-4o-mini \
MIMOCA_LLM_API_KEY=YOUR_KEY \
python3 python/service.py
```

Optional planner controls:
- `MIMOCA_LLM_TIMEOUT_S` (default `12`)
- `MIMOCA_LLM_TEMPERATURE` (default `0.2`)
- `MIMOCA_LLM_MAX_OUTPUT_CHARS` (default `220`)

The `/health` payload reports planner state flags:
- `planner_mode`
- `planner_provider`
- `planner_llm_configured`
- `planner_llm_ready`
- `planner_fallback_active`

## Windows release packaging (installable build)

The repository now includes a release workflow and installer project for Windows.

### Release artifact layout

The packaging job emits a deterministic `release/MiMoCA` tree before zipping/installer generation:

```text
release/MiMoCA/
├─ bin/
│  └─ mimoca.exe
├─ python/
│  ├─ service.py
│  ├─ requirements.txt
│  └─ bootstrap_sidecar_env.py
├─ runtime/
│  └─ python/                  # managed Python runtime copied from CI Python 3.11
├─ .mimoca_sidecar_venv/       # managed venv created during packaging
├─ assets/
│  └─ recipes.json
└─ scripts/
   ├─ first_launch_setup.ps1   # idempotent first-run bootstrap
   └─ launch_mimoca.bat        # Start Menu entry target
```

This satisfies the installable payload requirement of:
- main executable
- sidecar code
- managed Python runtime + venv
- required assets/bootstrap scripts

Model files are intentionally not hardbundled and are downloaded by the sidecar on first run (or first install run) into `%LOCALAPPDATA%\MiMoCA\model_cache`.

### Installer project (Inno Setup)

Installer source: `installer/mimoca.iss`.

Included behavior:
- installs full payload under `Program Files\MiMoCA`
- creates Start Menu entry (`MiMoCA`) and optional desktop shortcut
- runs `scripts/first_launch_setup.ps1` automatically during install
- optionally launches MiMoCA after install

### First-launch dependency/model setup

`launch_mimoca.bat` is the user-facing entrypoint used by the Start Menu shortcut. On every launch it:
1. runs `scripts/first_launch_setup.ps1` (idempotent)
2. ensures sidecar venv/config paths are present
3. sets `MIMOCA_MODEL_CACHE_ROOT=%LOCALAPPDATA%\MiMoCA\model_cache`
4. starts `bin\mimoca.exe`

At application startup, the sidecar still performs modality initialization and model download/warmup automatically when needed.

### Uninstall cleanup policy

Uninstall now removes transient local data by default:
- `%LOCALAPPDATA%\MiMoCA\logs`
- `%LOCALAPPDATA%\MiMoCA\temp`
- `%LOCALAPPDATA%\MiMoCA\app_data`
- `%LOCALAPPDATA%\MiMoCA\sidecar_venv`

During uninstall, the installer explicitly prompts whether to remove `%LOCALAPPDATA%\MiMoCA\model_cache`.
- **Yes**: full cleanup including model cache
- **No**: retains cached models for faster reinstall

### Build and generate installer

#### GitHub Actions (recommended)

Run workflow: `.github/workflows/windows-release.yml`

Triggers:
- manual (`workflow_dispatch`) with `version`
- tag push (`v*`)

Produced artifacts:
- `MiMoCA-<version>-windows-x64.zip`
- `MiMoCA-Setup-<version>.exe`

#### Local Windows build

1. Build app:

```powershell
cmake -S . -B build -DMIMOCA_ENABLE_QT_UI=OFF
cmake --build build --config Release
```

2. Create release layout (matching CI shape) and managed runtime/venv:

```powershell
$layout = "release/MiMoCA"
New-Item -ItemType Directory -Force -Path "$layout/bin", "$layout/runtime/python" | Out-Null
Copy-Item build/Release/mimoca.exe "$layout/bin/mimoca.exe" -Force
Copy-Item python "$layout/python" -Recurse -Force
Copy-Item assets "$layout/assets" -Recurse -Force
Copy-Item scripts "$layout/scripts" -Recurse -Force
Copy-Item "$env:pythonLocation/*" "$layout/runtime/python" -Recurse -Force
& "$layout/runtime/python/python.exe" "$layout/python/bootstrap_sidecar_env.py" --venv-path "$layout/.mimoca_sidecar_venv" --requirements "$layout/python/requirements.txt"
```

3. Build installer:

```powershell
$env:MIMOCA_VERSION = "0.1.0"
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" "installer/mimoca.iss"
```
