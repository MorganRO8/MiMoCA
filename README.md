# README.md

# Multimodal Cooking Assistant

A **Windows-first multimodal cooking assistant prototype** built around a **C++ desktop app** with a **minimal Python sidecar**.

## Current vertical slice

This repo now includes:
- C++ shell app (`src/main.cpp`)
- Python sidecar (`python/service.py`) with:
  - `GET /health`
  - `POST /plan` mock planner using the canonical `TurnContext` and `PlannerResponse` contracts
- Recipe asset file (`assets/recipes.json`) with one real branching demo (`rice_cooker` vs `pot`)

The C++ app now runs a tiny end-to-end path:
1. load one recipe from `assets/recipes.json`
2. track current step and selected recipe branches in memory
3. health-check sidecar
4. accept utterances through the Python STT boundary by sending buffered WAV audio (`stt-file <path.wav>`) or shortcut commands (`current`, `next`)
5. accept debug gesture injection (`gesture <label> [confidence]`) with tiny vocabulary: `next`, `repeat`, `option_a`, `option_b`, `none`
6. send turn context to `POST /plan` including `user_utterance` plus `gesture.label` and `gesture.confidence`
7. transcribe audio in the sidecar with `faster-whisper` (CTranslate2 backend) and return transcript text to the app
8. parse `PlannerResponse`
9. speak `assistant_text` with Windows SAPI when `speak` is true
10. select and persist branch choices from utterance (`rice cooker` / `pot`) or gestures (`option_a` / `option_b`)
11. advance local step if `advance_step` is true, using branch-specific next-step routing when a branch is selected
12. start camera capture on device `0` and keep a latest-frame summary (availability, width/height, timestamp, frame count) for planner turns
13. optionally emit compact debug snapshots (transcript, gesture, detections, recipe/step/branch, planner round-trip status)

Both C++ and Python log serialized planner request/response JSON at the service boundary.
The C++ app also logs camera lifecycle events (start/stop/first-frame availability).

## Build and run (minimal)

### 1) Build C++ app

```bash
cmake -S . -B build
cmake --build build
```

### 2) Start Python sidecar

From repo root:

```bash
python3 python/service.py
```

Install Python dependencies first:

```bash
python3 -m pip install -r python/requirements.txt
```

The sidecar listens on `http://127.0.0.1:8080`.

### 3) Run C++ app

```bash
./build/mimoca
```

On startup, the app loads the sample recipe and then accepts these commands:
- `current` → ask planner for current step
- `next` → ask planner for next instruction (and advance if available)
- `gesture <label> [confidence]` → run a gesture-only planner turn using one of: `next`, `repeat`, `option_a`, `option_b`, `none`
- `stt-file ./path/to/audio.wav` → transcribe a buffered WAV clip using faster-whisper and run a planner turn
- `camera` → print camera status and latest frame summary
- `stop` → cancel current TTS playback (temporary manual interruption control)
- `debug on` / `debug off` / `debug status` → enable, disable, or print compact debug state
- `exit` → quit

To choose a different model/device at startup:

```bash
MIMOCA_STT_MODEL=large-v3 MIMOCA_STT_DEVICE=cpu python3 python/service.py
```

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
- TTS interruption is currently manual via `stop`; microphone-driven auto interruption is not wired yet.
- The sidecar boundary is intentionally small and logged to keep iteration fast.
- Recipe parsing is intentionally minimal and currently targets one startup recipe.
