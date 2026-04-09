# AGENTS.md

## Mission

Build a **Windows-first multimodal cooking assistant** that helps a user cook hands-free using:
- camera-based ingredient awareness
- hand/gesture recognition
- speech recognition for interruption and questions
- LLM-based step planning
- text-to-speech feedback
- optional AR overlays later

The goal is to keep the repository small and understandable. Prefer a **minimal file count** and a **vertical-slice prototype** over broad scaffolding.

---

## Product Scope

### Primary user experience
The user opens the app on Windows, points a camera at their workspace, and interacts mostly by voice and simple gestures. The assistant should:
1. understand the current recipe step
2. observe the workspace
3. answer spoken questions
4. detect likely ingredients/tools in view
5. optionally recognize a small set of cooking gestures
6. speak concise responses
7. allow branch selection in a recipe flow without touching the UI

### Prototype success criteria
A usable prototype is successful if it can:
- load one or two structured recipes
- accept microphone input
- transcribe user speech locally or through a configurable backend
- capture camera frames
- identify a limited ingredient/tool set from those frames
- classify a limited gesture vocabulary
- assemble multimodal context
- ask an LLM for the next response
- speak the answer aloud
- interrupt TTS when the user starts talking
- switch recipe branches using gesture or spoken choice

### Explicit non-goals for v1
Do not try to fully solve:
- robust chef-grade hand-pose coaching
- production-grade AR
- large recipe marketplaces
- cloud orchestration
- multi-user sync
- full offline training pipelines inside this repo

---

## Architecture Decision

## Chosen approach: Hybrid C++ host + Python inference sidecar

Targeting **Windows + C++ is feasible**, but a pure C++ stack is likely to create friction for multimodal inference because many practical speech/CV/LLM integrations are Python-centric. For the prototype, use:

- **C++** for the main desktop app, UI shell, device control, and low-latency orchestration
- **Python** for model-heavy services where ecosystem maturity is better
- a **small local bridge** between the two, preferably HTTP on localhost or stdio JSON-RPC
- structured JSON messages for all cross-language requests

This gives the project:
- a Windows-native app path
- access to mature Python inference libraries
- clean separation between interaction logic and model runtime
- easier future substitution of Python components with C++/ONNX where beneficial

### Recommended system split

#### C++ app responsibilities
- camera capture
- microphone streaming control
- app windows and settings UI
- recipe state machine
- turn assembly
- branch selection logic
- interruption handling
- local persistence and logging
- local HTTP or process management for Python services

#### Python service responsibilities
- speech-to-text inference adapter
- vision model adapter for ingredient/tool detection
- gesture recognition adapter
- optional TTS adapter if C++ TTS is not used initially
- multimodal prompt construction helper
- LLM backend adapter abstraction

---

## Repo Philosophy

Keep the repo intentionally small.

### File-count rule
Prefer putting detail into a few high-value files instead of many thin files. Avoid speculative abstractions. Every new file should justify itself.

### Suggested eventual repository shape
This is a target shape, not a command to create everything immediately:

```text
/
├─ README.md
├─ AGENTS.md
├─ CMakeLists.txt
├─ src/
│  └─ main.cpp
├─ python/
│  └─ service.py
├─ assets/
│  └─ recipes.json
└─ third_party/   (only if truly necessary)
```

That is enough for a real prototype. Expand only when pain becomes clear.

---

## Core Interaction Loop

For each assistant turn:

1. Capture current app state:
   - current recipe
   - current step
   - chosen branch
   - enabled features
2. Gather live signals:
   - latest speech transcript
   - latest frame or frame summary
   - latest detected ingredients/tools
   - latest gesture classification
3. Build a normalized **TurnContext** object
4. Send TurnContext to the planner
5. Planner returns:
   - spoken response
   - visual highlights or UI hints
   - branch updates if any
   - confidence / fallback markers
6. App speaks response
7. If user begins talking, stop TTS immediately and start new turn

---

## Canonical Data Contracts

### TurnContext
```json
{
  "timestamp": "ISO-8601",
  "recipe_id": "string",
  "step_id": "string",
  "branch_id": "string|null",
  "user_utterance": "string",
  "gesture": {
    "label": "next|repeat|option_a|option_b|none",
    "confidence": 0.0
  },
  "detections": [
    {
      "label": "knife",
      "confidence": 0.93,
      "bbox": [0.1, 0.2, 0.3, 0.4]
    }
  ],
  "hand_pose": {
    "label": "pinch_grip|safe_claw|unknown",
    "confidence": 0.0
  },
  "settings": {
    "speech_enabled": true,
    "vision_enabled": true,
    "gesture_enabled": true,
    "tts_enabled": true
  }
}
```

### PlannerResponse
```json
{
  "assistant_text": "Chop the onion into small pieces.",
  "speak": true,
  "interruptible": true,
  "advance_step": false,
  "new_branch_id": null,
  "ui_overlays": [
    {
      "type": "highlight_label",
      "target": "onion"
    }
  ]
}
```

### Recipe format
Recipes should be stored in a compact structured format with:
- metadata
- ingredients
- tools
- steps
- branch points
- prompts or hints for recovery

---

## Technology Recommendations

### C++ app
- language: **C++20**
- build: **CMake**
- package manager: **vcpkg** on Windows
- GUI:
  - preferred: **Qt**
  - acceptable for bare prototype: minimal native or immediate-mode UI
- camera/image processing: **OpenCV**
- HTTP client/server or child-process bridge
- JSON: a modern lightweight JSON library
- logging: simple structured logging to file

### Python sidecar
- Python 3.11+ preferred
- FastAPI or Flask if using localhost HTTP
- adapters around model/runtime choices
- keep import surface narrow and configurable

### LLM integration
Abstract the planner backend so it can be swapped:
- remote API-backed multimodal planner
- local text-only planner for offline fallback
- mocked planner for test mode

### TTS
Choose the fastest route to a good demo:
- Windows-native speech first if latency and voice quality are acceptable
- Python TTS backend only if needed for better voice control

### Vision
Start small:
- detect a tiny curated vocabulary of ingredients and cookware
- detect one hand in frame
- classify a few gestures only

---

## Phase Plan

## Phase 0 — Vertical-slice definition
Deliverable:
- one recipe
- one camera feed
- one speech loop
- one planner response path
- one TTS response path

Exit criteria:
- user can ask “what next?”
- assistant answers based on current recipe step

## Phase 1 — Basic multimodal wiring
Deliverable:
- C++ app launches Python service
- camera frames reach Python
- speech transcript reaches planner
- planner response returns to app

Exit criteria:
- turn loop is stable for 5+ minutes without restart

## Phase 2 — Ingredient/tool awareness
Deliverable:
- limited object recognition
- object labels visible in debug mode
- planner can reference visible items

Exit criteria:
- assistant can say things like “I see the cutting board and onion”

## Phase 3 — Gesture control
Deliverable:
- small gesture set:
  - next
  - repeat
  - choose left option
  - choose right option
  - pause
- branch selection for at least one recipe

Exit criteria:
- user can progress without touching controls

## Phase 4 — Better recipe intelligence
Deliverable:
- structured recipe branches
- recovery prompts
- substitution/help questions

Exit criteria:
- assistant handles alternate cooking paths cleanly

## Phase 5 — Hand-pose coaching
Deliverable:
- experimental hand-pose labels for one or two knife techniques
- warning/fallback messages only

Exit criteria:
- assistant can cautiously identify likely safe/unsafe pose states

## Phase 6 — Optional AR
Only start if earlier phases are solid.

---

## Coding Rules for Codex

When generating code for this repo, follow these rules:

1. **Favor the smallest workable implementation.**
2. Do not add extra folders unless they directly reduce complexity.
3. Keep UI, orchestration, and state transitions easy to trace.
4. Prefer explicit code over meta-framework magic.
5. Keep Python service endpoints minimal and stable.
6. Add logging around every cross-language boundary.
7. Make every model-backed component replaceable by a mock.
8. Design for graceful degradation when a modality is unavailable.
9. Windows-first always wins over cross-platform elegance for the first prototype.

---

## Error Handling Expectations

The app should never fail hard because one modality is missing.

### Examples
- camera unavailable → continue with voice-only mode
- microphone unavailable → continue with tap/manual control mode
- vision model timeout → use recipe-only planning
- LLM unavailable → fall back to deterministic recipe hints
- TTS failure → show text response only

---

## Safety and UX Constraints

Because this is a cooking assistant:
- never present uncertain detections as facts
- phrase risky guidance conservatively
- do not claim food safety certainty without explicit rules/data
- prefer short, clear, spoken-friendly responses

---

## Testing Strategy

### Minimal tests worth having
- recipe parser test
- TurnContext serialization test
- planner mock response test
- interruption state transition test
- Python bridge health check
- one end-to-end smoke test

### Demo scenarios
- “What do I do next?”
- “Repeat that.”
- “Can I use a pot instead of a rice cooker?”
- gesture chooses recipe branch
- user interrupts TTS with a question
- ingredient not detected, app recovers gracefully

---

## Configuration Strategy

Keep config centralized and human-readable.

Suggested config domains:
- model/service endpoints
- enabled modalities
- recipe asset path
- debug flags
- thresholds for gesture/object confidence
- privacy/logging controls

Use environment variables for secrets and a small local config file for non-secret settings.

---

## Observability

Include:
- app log
- Python service log
- optional debug overlay showing:
  - latest transcript
  - latest gesture
  - latest detections
  - planner round-trip time

---

## First Build Order for Codex

When asked to start implementation, Codex should proceed in this order:

1. Create a tiny CMake-based C++ desktop shell
2. Add camera preview and microphone state plumbing
3. Add Python service with health endpoint and mock planner
4. Define TurnContext and PlannerResponse JSON contracts
5. Wire one recipe JSON file
6. Implement speech transcript input path
7. Add TTS output with interruption
8. Add mock gesture input
9. Add real vision/gesture adapters behind interfaces
10. Add branching recipe demo

Do not begin with model training or AR.

---

## Definition of Done for the Prototype

The prototype is done when a Windows user can:
- launch the app
- choose a sample recipe
- see camera feed
- ask spoken questions
- hear spoken answers
- navigate at least one recipe branch
- receive responses that reflect recipe state plus at least one live modality
