# README.md

# Multimodal Cooking Assistant

A **Windows-first multimodal cooking assistant prototype** built around a **C++ desktop app** with a **minimal Python sidecar**.

## Current vertical slice

This repo now includes:
- C++ shell app (`src/main.cpp`)
- Python sidecar (`python/service.py`) with:
  - `GET /health`
  - `POST /plan` (mock response)

The C++ app attempts a localhost health check at startup and logs whether the sidecar is available.

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

On startup, the app checks `http://127.0.0.1:8080/health` and logs success/fallback.

## Manual health-path testing

### Health check

```bash
curl http://127.0.0.1:8080/health
```

Expected: JSON with `status: "ok"`.

### Minimal mock planner path

```bash
curl -X POST http://127.0.0.1:8080/plan \
  -H "Content-Type: application/json" \
  -d '{"recipe_id":"demo","step_id":"step_1","user_utterance":"what next?"}'
```

Expected: a mock `PlannerResponse` JSON payload.

## Notes

- No real model integrations are included yet.
- The sidecar boundary is intentionally small and logged to keep iteration fast.
