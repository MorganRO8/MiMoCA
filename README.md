# README.md

# Multimodal Cooking Assistant

A **Windows-first multimodal cooking assistant prototype** built around a **C++ desktop app** with **Python-backed multimodal services** for speech, vision, and planning.

This implementation plan is grounded in the uploaded presentation, which outlines hand recognition, speech recognition, ingredient recognition, user settings, LLM-based planning, branched-path recipes, TTS, and optional AR.

## Why this architecture?

The project idea naturally spans:
- real-time camera input
- microphone input
- gesture recognition
- ingredient/tool detection
- LLM-based reasoning
- spoken output

A pure C++ implementation is possible for parts of this, but a **hybrid design** is the most practical place to start:

- **C++** handles the Windows desktop experience, app state, low-latency orchestration, and device control
- **Python** handles model-heavy backend tasks where tooling is stronger and iteration is faster

This keeps the prototype achievable while still preserving a path toward a more native deployment later.

---

## Project goals

The prototype should help a user cook hands-free by:
- following a structured recipe
- answering “what next?” style questions
- using speech as the primary input
- detecting a small number of relevant objects or ingredients
- recognizing a limited set of gestures
- reading responses aloud
- allowing branch selection between alternate cooking paths

### Example branch
A recipe might ask whether the user wants to cook rice:
- in a rice cooker
- or in a pot

The app should let the user choose by voice or gesture and then continue down the selected branch.

---

## Scope for the first implementation

### In scope
- Windows desktop prototype
- C++20 + CMake
- one or two recipes stored in a structured data file
- camera feed
- microphone input
- speech transcription
- limited ingredient/tool recognition
- limited gesture recognition
- LLM-backed or mock planning
- TTS with interruption
- settings for turning modalities on and off

### Out of scope for the initial prototype
- polished AR
- broad recipe catalog
- cloud backend infrastructure
- perfect hand-technique coaching
- full offline model training workflows
- production deployment hardening

---

## Proposed architecture

```text
+--------------------------------------------------------------+
|                      Windows Desktop App                      |
|                           (C++)                              |
|--------------------------------------------------------------|
| UI | Recipe State | Camera Control | Mic Control | TTS Ctrl |
| Turn Assembly | Settings | Logging | Python Proc Manager     |
+----------------------------|---------------------------------+
                             |
                             | localhost HTTP or stdio JSON
                             v
+--------------------------------------------------------------+
|                     Multimodal Service                        |
|                           (Python)                           |
|--------------------------------------------------------------|
| STT Adapter | Vision Adapter | Gesture Adapter | Planner    |
| Prompt Builder | Fallback Logic | Optional TTS Adapter      |
+--------------------------------------------------------------+
```

### Rationale
This split gives the app:
- a clean Windows-native surface
- easier access to model ecosystems
- a controlled boundary between UI/orchestration and inference
- simpler mocking for development

---

## Planned user flow

1. User launches the app.
2. User selects a recipe.
3. App opens camera preview and microphone pipeline.
4. Assistant tracks the current recipe step.
5. User asks a question such as “What do I do next?”
6. App packages:
   - current recipe state
   - latest speech transcript
   - latest detections
   - latest gesture result
7. Planner returns the next response.
8. App displays and speaks it.
9. If the user talks while TTS is speaking, playback stops and the next turn begins.

---

## Multimodal features

## 1. Speech recognition
Speech drives the main interaction loop. It also enables interruption while the assistant is speaking.

### Prototype target
- stable transcription for short cooking questions
- partial/final transcript handling
- interruption event when the user begins speaking

## 2. Ingredient and cookware recognition
The app should identify a small curated vocabulary of visible items and pass those observations into the planner.

### Prototype target
- detect a few ingredients and tools reliably in controlled conditions
- expose results in debug mode
- allow planner to reference what is visible

## 3. Hand / gesture recognition
The prototype should begin with a small vocabulary:
- next
- repeat
- option A
- option B
- pause

Do not overbuild here. Gesture reliability matters more than variety.

## 4. LLM-based planning
The planner receives:
- screenshots or frame summaries
- recognized objects
- hand/gesture information
- user speech
- recipe context

It then returns short, spoken-friendly instructions.

## 5. Text-to-speech
Responses should be spoken aloud and stop immediately when the user interrupts.

## 6. Optional AR
AR should remain deferred until the non-AR interaction loop feels good.

---

## Data model plan

To avoid file sprawl, keep the data model compact and explicit.

### Turn context
Every interaction turn should combine:
- recipe ID
- current step ID
- branch ID
- user utterance
- latest gesture label/confidence
- latest detections
- enabled settings
- timestamps/debug metadata

### Planner response
The planner should return:
- assistant text
- whether to speak it
- whether it can be interrupted
- whether to advance the recipe
- any branch updates
- optional UI overlay hints

### Recipe structure
Recipes should support:
- ingredients
- tools
- ordered steps
- branch points
- short recovery/help prompts

---

## Build plan

## Phase 0 — skeleton
Build only enough to prove the architecture:
- C++ app shell
- Python service shell
- health check
- one hardcoded recipe
- mock planner response

## Phase 1 — turn loop
Add:
- camera preview
- transcript input path
- TurnContext JSON
- PlannerResponse JSON
- TTS output

## Phase 2 — multimodal awareness
Add:
- limited object detection
- limited gesture detection
- multimodal planner input
- debug overlay

## Phase 3 — branchable recipes
Add:
- recipe branch representation
- spoken branch confirmation
- gesture-based branch selection

## Phase 4 — safer, better UX
Add:
- graceful fallbacks
- confidence-aware language
- modality toggles
- logging and diagnostics

## Phase 5 — optional hand-pose coaching
Only after the above works.

---

## Minimal repository shape

The repo should stay compact:

```text
/
├─ README.md
├─ AGENTS.md
├─ CMakeLists.txt
├─ src/
│  └─ main.cpp
├─ python/
│  └─ service.py
└─ assets/
   └─ recipes.json
```

This is intentionally small. More files should be added only when they clearly simplify the codebase.

---

## Recommended implementation choices

### C++ side
- **C++20**
- **CMake**
- **vcpkg** for Windows dependency management
- **Qt** if a real desktop UI is needed early
- **OpenCV** for camera/image plumbing
- structured JSON messaging between app and Python service

### Python side
- lightweight local service
- small adapters around:
  - speech recognition
  - vision detection
  - gesture recognition
  - planner backend
- mock mode for every model-dependent feature

### Why mock mode matters
This project spans several uncertain or evolving components. Mock mode lets the team:
- demo the interaction loop early
- develop the UI before models are final
- isolate failures quickly
- keep progress moving when one modality lags

---

## Reliability and fallback rules

The assistant should degrade gracefully.

### Fallback behavior examples
- no camera → continue with recipe + speech only
- no microphone → offer UI/manual control
- no vision result → do not invent observations
- no planner response → provide deterministic recipe text
- no TTS → display text only

### Style rule
When uncertain, the assistant should say so plainly.

---

## What Codex should build first

When starting implementation, the first milestone should be a **vertical slice**, not a full framework.

### First milestone
A Windows app where the user can:
- open a sample recipe
- ask “what next?”
- get a response spoken aloud
- interrupt the response by speaking

### Second milestone
Add:
- simple object labels
- one gesture-driven branch
- one debug panel showing latest multimodal state

### Third milestone
Replace mocks with better adapters one at a time.

---

## Success criteria

The prototype is successful when it can demonstrate all of the following in a single session:

- recipe selection
- current-step tracking
- voice question input
- spoken answer output
- TTS interruption by user speech
- one branching recipe path
- at least one live visual signal influencing the answer

---

## Summary

This project should begin as a **compact hybrid prototype**:
- **Windows desktop app in C++**
- **Python sidecar for multimodal inference**
- **minimal repository structure**
- **clear data contracts**
- **vertical-slice delivery first**

That approach matches the project vision while keeping implementation risk under control.
