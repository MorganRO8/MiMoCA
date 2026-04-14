# MiMoCA Manual QA Script (Voice + Gesture End-to-End)

This script validates release readiness for the full multimodal flow.

## 0) Prepare report file

```bash
python3 scripts/manual_qa_scenario.py --init --tester "<name>"
```

Edit `artifacts/readiness/manual_qa_report.json` as each step passes.

## 1) Fresh install succeeds

1. Install from `MiMoCA-Setup-<version>.exe` on a clean Windows profile/VM.
2. Confirm Start Menu shortcut launches app without manual dependency setup.
3. Mark `fresh_install_succeeds=true`.

## 2) Sidecar auto-start

1. Launch using Start Menu shortcut (calls `scripts/launch_mimoca.bat`).
2. Verify sidecar reaches healthy state via app status UI.
3. Mark `sidecar_auto_start=true`.

## 3) Model prefetch complete

1. Wait until startup readiness indicates required modalities are ready.
2. Confirm STT + vision model readiness in app/sidecar health details.
3. Mark `model_prefetch_complete=true`.

## 4) API key prompt + secure save

1. Set planner mode to `llm` with no existing saved key.
2. Confirm API key prompt appears.
3. Enter a valid key and verify key is accepted.
4. Confirm key is **not** stored in plaintext config file and is persisted in Windows secure storage path.
5. Mark `api_key_prompt_and_secure_save=true`.

## 5) Live microphone conversation + TTS interruption

1. Ask: “What do I do next?”
2. Confirm assistant responds by voice.
3. Speak while TTS is playing and confirm TTS stops immediately.
4. Confirm new utterance is processed.
5. Mark `live_mic_conversation_with_tts_interrupt=true`.

## 6) Camera preview + gesture branch selection

1. Confirm camera preview is visible.
2. Reach a recipe branch prompt.
3. Use gesture `option_a` or `option_b`.
4. Confirm recipe branch updates and next step instruction reflects branch choice.
5. Mark `camera_preview_and_gesture_branch_selection=true`.

## 7) Validate report

```bash
python3 scripts/manual_qa_scenario.py --validate
```

Release gate requires this validation to pass.
