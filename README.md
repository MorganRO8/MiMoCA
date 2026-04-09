# README.md

# Multimodal Cooking Assistant

A **Windows-first multimodal cooking assistant prototype** built around a **C++ desktop app** with a **minimal Python sidecar**.

## Current vertical slice

This repo now includes:
- C++ shell app (`src/main.cpp`)
- Python sidecar (`python/service.py`) with:
  - `GET /health`
  - `POST /plan` mock planner using the canonical `TurnContext` and `PlannerResponse` contracts
- Recipe asset file (`assets/recipes.json`) with one sample recipe and a future branch point shape

The C++ app now runs a tiny end-to-end path:
1. load one recipe from `assets/recipes.json`
2. track current step in memory
3. health-check sidecar
4. accept utterances from a minimal speech adapter boundary (`say-partial ...`, `say-final ...`) or shortcut commands (`current`, `next`)
5. accept debug gesture injection (`gesture <label> [confidence]`) with tiny vocabulary: `next`, `repeat`, `option_a`, `option_b`, `none`
6. send turn context to `POST /plan` including `user_utterance` plus `gesture.label` and `gesture.confidence`
7. log partial/final transcript events in the app and log utterance receipt in the sidecar
8. parse `PlannerResponse`
9. speak `assistant_text` with Windows SAPI when `speak` is true
10. advance local step if `advance_step` is true
11. start camera capture on device `0` and keep a latest-frame summary (availability, width/height, timestamp, frame count) for planner turns

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

The sidecar listens on `http://127.0.0.1:8080`.

### 3) Run C++ app

```bash
./build/mimoca
```

On startup, the app loads the sample recipe and then accepts these commands:
- `current` → ask planner for current step
- `next` → ask planner for next instruction (and advance if available)
- `gesture <label> [confidence]` → run a gesture-only planner turn using one of: `next`, `repeat`, `option_a`, `option_b`, `none`
- `camera` → print camera status and latest frame summary
- `say-partial <text>` → emit a mock partial transcript event (logged only)
- `say-final <text>` → emit a mock final transcript event and run a planner turn with that utterance
- `stop` → cancel current TTS playback (temporary manual interruption control)
- `exit` → quit

If OpenCV is available at build time, camera capture is enabled automatically. If OpenCV is not found, the app stays in graceful camera-disabled mode and still runs speech + planner flow.

## Manual health-path testing

### Health check

```bash
curl http://127.0.0.1:8080/health
```

Expected: JSON with `status: "ok"`.

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

- No real model integrations are included yet.
- Speech input is currently a mock transcript adapter over stdin (`say-partial` / `say-final`), intentionally separated so it can be replaced with a Windows microphone-backed adapter later without changing planner wiring.
- TTS interruption is currently manual via `stop`; microphone-driven auto interruption is not wired yet.
- The sidecar boundary is intentionally small and logged to keep iteration fast.
- Recipe parsing is intentionally minimal and currently targets one startup recipe.
