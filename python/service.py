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

        logging.info("plan payload keys=%s", sorted(turn_context.keys()))
        self._write_json(
            200,
            {
                "assistant_text": "Mock planner: ask me 'what next?' once recipe wiring is in place.",
                "speak": True,
                "interruptible": True,
                "advance_step": False,
                "new_branch_id": None,
                "ui_overlays": [],
            },
        )


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
