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

### 3) Run C++ app

```bash
./build/mimoca
```

On startup, the app loads the sample recipe and then accepts these commands:
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
- If the YOLO model path points to an ONNX model, Ultralytics can run it with ONNX Runtime (`onnxruntime` dependency included).
- The sidecar boundary is intentionally small and logged to keep iteration fast.
- Recipe parsing is intentionally minimal and currently targets one startup recipe.
