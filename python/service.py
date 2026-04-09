#!/usr/bin/env python3
"""Minimal MiMoCA Python sidecar service.

Endpoints:
- GET /health -> service status JSON
- POST /plan -> mock planner response JSON
"""

from __future__ import annotations

import json
import logging
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = 8080


def _deterministic_mock_planner(turn_context: dict) -> dict:
    utterance = str(turn_context.get("user_utterance", "")).strip().lower()
    raw_utterance = str(turn_context.get("user_utterance", "")).strip()
    gesture_label = str(turn_context.get("gesture", {}).get("label", "none")).strip().lower()
    try:
        gesture_confidence = float(turn_context.get("gesture", {}).get("confidence", 0.0) or 0.0)
    except (TypeError, ValueError):
        gesture_confidence = 0.0
    current_step_instruction = str(turn_context.get("current_step_instruction", "")).strip()
    next_step_instruction = str(turn_context.get("next_step_instruction", "")).strip()
    gesture_present = gesture_label in {"next", "repeat", "option_a", "option_b"} and gesture_confidence > 0.0

    if gesture_present and gesture_label == "next":
        if next_step_instruction:
            assistant_text = f"Gesture next: {next_step_instruction}"
            advance_step = True
            target = "next_step"
        else:
            assistant_text = "Gesture next received, but you are at the final step."
            advance_step = False
            target = "recipe_complete"
        new_branch_id = None
    elif gesture_present and gesture_label == "repeat":
        assistant_text = (
            f"Gesture repeat: {current_step_instruction}"
            if current_step_instruction
            else "Gesture repeat received, but current step is unavailable."
        )
        advance_step = False
        target = "current_step"
        new_branch_id = None
    elif gesture_present and gesture_label == "option_a":
        assistant_text = "Gesture selected option A branch."
        advance_step = False
        target = "branch_option_a"
        new_branch_id = "option_a"
    elif gesture_present and gesture_label == "option_b":
        assistant_text = "Gesture selected option B branch."
        advance_step = False
        target = "branch_option_b"
        new_branch_id = "option_b"
    elif "current step" in utterance:
        assistant_text = (
            f"Current step: {current_step_instruction}"
            if current_step_instruction
            else "Current step is not available."
        )
        advance_step = False
        target = "current_step"
        new_branch_id = None
    elif "what next" in utterance or gesture_label == "next":
        if next_step_instruction:
            assistant_text = f"Next instruction: {next_step_instruction}"
            advance_step = True
            target = "next_step"
        else:
            assistant_text = "You are at the final step."
            advance_step = False
            target = "recipe_complete"
        new_branch_id = None
    elif "repeat" in utterance or gesture_label == "repeat":
        assistant_text = (
            f"Repeat: {current_step_instruction}"
            if current_step_instruction
            else "I do not have a step to repeat yet."
        )
        advance_step = False
        target = "current_step"
        new_branch_id = None
    else:
        if raw_utterance:
            assistant_text = (
                f"I heard: '{raw_utterance}'. Ask for 'current step' or 'what next?'."
            )
        else:
            assistant_text = "Mock planner ready. Use voice or gesture: next, repeat, option_a, option_b."
        advance_step = False
        target = "recipe"
        new_branch_id = None

    return {
        "assistant_text": assistant_text,
        "speak": True,
        "interruptible": True,
        "advance_step": advance_step,
        "new_branch_id": new_branch_id,
        "ui_overlays": [{"type": "highlight_label", "target": target}],
    }


class Handler(BaseHTTPRequestHandler):
    def _write_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        logging.info("%s - %s", self.address_string(), fmt % args)

    def do_GET(self) -> None:
        logging.info("request GET %s", self.path)
        if self.path == "/health":
            self._write_json(
                200,
                {
                    "status": "ok",
                    "service": "mimoca-python-sidecar",
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                },
            )
            return

        self._write_json(404, {"error": "not_found", "path": self.path})

    def do_POST(self) -> None:
        logging.info("request POST %s", self.path)
        if self.path != "/plan":
            self._write_json(404, {"error": "not_found", "path": self.path})
            return

        content_length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(content_length) if content_length > 0 else b"{}"

        try:
            turn_context = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            self._write_json(400, {"error": "invalid_json"})
            return

        logging.info("serialized TurnContext request: %s", json.dumps(turn_context, separators=(",", ":")))
        logging.info("planner received user_utterance: %s", turn_context.get("user_utterance", ""))
        planner_response = _deterministic_mock_planner(turn_context)
        logging.info("serialized PlannerResponse response: %s", json.dumps(planner_response, separators=(",", ":")))
        self._write_json(200, planner_response)


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="[python-sidecar] %(asctime)s %(levelname)s %(message)s",
    )
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    logging.info("service listening on http://%s:%d", HOST, PORT)
    server.serve_forever()


if __name__ == "__main__":
    main()
