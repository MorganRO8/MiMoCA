#!/usr/bin/env python3
"""Minimal MiMoCA Python sidecar service.

Endpoints:
- GET /health -> service status JSON
- POST /plan -> mock planner response JSON
- POST /stt/transcribe -> faster-whisper transcription for buffered audio
- POST /stt/session/start|chunk|finalize -> chunk buffering API for future streaming
- POST /gesture/detect -> MediaPipe Hand Landmarker + tiny gesture mapping
"""

from __future__ import annotations

import json
import logging
import base64
import io
import os
import uuid
import wave
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dataclasses import dataclass, field

import numpy as np
import cv2
import mediapipe as mp
from faster_whisper import WhisperModel
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

HOST = "127.0.0.1"
PORT = 8080
DEFAULT_STT_MODEL = os.getenv("MIMOCA_STT_MODEL", "distil-large-v3")
DEFAULT_STT_DEVICE = os.getenv("MIMOCA_STT_DEVICE", "cpu")
DEFAULT_STT_COMPUTE_TYPE = os.getenv("MIMOCA_STT_COMPUTE_TYPE", "int8")
DEFAULT_GESTURE_MODEL_PATH = os.getenv("MIMOCA_GESTURE_MODEL_PATH", "python/models/hand_landmarker.task")


@dataclass
class BufferedAudioSession:
    sample_rate_hz: int
    audio_bytes: bytearray = field(default_factory=bytearray)
    utterance_audio_bytes: bytearray = field(default_factory=bytearray)
    utterance_texts: list[str] = field(default_factory=list)
    vad_state: "SimpleVadState" | None = None


@dataclass
class SimpleVadState:
    speech_active: bool = False
    consecutive_speech_frames: int = 0
    consecutive_silence_frames: int = 0
    noise_rms: float = 0.005
    speech_started_once: bool = False
    failure_reason: str = ""

    @property
    def available(self) -> bool:
        return self.failure_reason == ""


class FasterWhisperSpeechAdapter:
    """Small adapter boundary around faster-whisper."""

    def __init__(self) -> None:
        self.model_name = DEFAULT_STT_MODEL
        self.device = DEFAULT_STT_DEVICE
        self.compute_type = DEFAULT_STT_COMPUTE_TYPE
        self.model = self._load_model_with_fallback()
        self.sessions: dict[str, BufferedAudioSession] = {}
        self.vad_frame_ms = int(os.getenv("MIMOCA_VAD_FRAME_MS", "20"))
        self.vad_start_frames = int(os.getenv("MIMOCA_VAD_START_FRAMES", "2"))
        self.vad_end_frames = int(os.getenv("MIMOCA_VAD_END_FRAMES", "15"))
        self.vad_min_rms = float(os.getenv("MIMOCA_VAD_MIN_RMS", "0.008"))
        self.vad_noise_multiplier = float(os.getenv("MIMOCA_VAD_NOISE_MULTIPLIER", "2.8"))
        self.vad_history = deque(maxlen=64)

    def _load_model_with_fallback(self) -> WhisperModel:
        candidates = [self.model_name]
        if self.model_name == "distil-large-v3":
            candidates.append("large-v3")
        last_error: Exception | None = None
        for candidate in candidates:
            try:
                model = WhisperModel(candidate, device=self.device, compute_type=self.compute_type)
                self.model_name = candidate
                logging.info("STT model loaded: %s (device=%s, compute_type=%s)", candidate, self.device, self.compute_type)
                return model
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                logging.warning("STT model load failed for %s: %s", candidate, exc)
        raise RuntimeError(f"Failed to load any STT model candidate: {candidates}") from last_error

    @staticmethod
    def _decode_audio_bytes(audio_base64: str) -> bytes:
        try:
            return base64.b64decode(audio_base64)
        except Exception as exc:  # noqa: BLE001
            raise ValueError("invalid_audio_base64") from exc

    @staticmethod
    def _pcm_s16le_to_float32_mono(audio_bytes: bytes) -> np.ndarray:
        if not audio_bytes:
            return np.array([], dtype=np.float32)
        audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
        return (audio_i16.astype(np.float32) / 32768.0).flatten()

    @staticmethod
    def _wav_bytes_to_float32_mono(audio_bytes: bytes) -> tuple[np.ndarray, int]:
        with wave.open(io.BytesIO(audio_bytes), "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_rate_hz = wav_file.getframerate()
            sample_width = wav_file.getsampwidth()
            frames = wav_file.readframes(wav_file.getnframes())
        if sample_width != 2:
            raise ValueError("unsupported_wav_sample_width")
        audio_i16 = np.frombuffer(frames, dtype=np.int16)
        if channels > 1:
            audio_i16 = audio_i16.reshape(-1, channels).mean(axis=1).astype(np.int16)
        return audio_i16.astype(np.float32) / 32768.0, sample_rate_hz

    @staticmethod
    def _safe_int16_frames(audio_bytes: bytes) -> np.ndarray:
        if not audio_bytes:
            return np.array([], dtype=np.int16)
        aligned = audio_bytes[: len(audio_bytes) - (len(audio_bytes) % 2)]
        if not aligned:
            return np.array([], dtype=np.int16)
        return np.frombuffer(aligned, dtype=np.int16)

    def _vad_from_pcm_chunk(self, state: SimpleVadState, sample_rate_hz: int, audio_chunk: bytes) -> dict:
        frame_samples = max(1, int(sample_rate_hz * (self.vad_frame_ms / 1000.0)))
        if sample_rate_hz < 8000 or sample_rate_hz > 48000:
            state.failure_reason = "unsupported_sample_rate_for_vad"
            return {"speech_started": False, "speech_ended": False, "speech_active": False, "vad_available": False}
        frames = self._safe_int16_frames(audio_chunk).astype(np.float32) / 32768.0
        if frames.size == 0:
            return {
                "speech_started": False,
                "speech_ended": False,
                "speech_active": state.speech_active,
                "vad_available": state.available,
                "rms": 0.0,
                "threshold": max(self.vad_min_rms, state.noise_rms * self.vad_noise_multiplier),
            }

        speech_votes = 0
        total_votes = 0
        rms_values: list[float] = []
        for start in range(0, frames.size, frame_samples):
            frame = frames[start : start + frame_samples]
            if frame.size == 0:
                continue
            rms = float(np.sqrt(np.mean(frame * frame)))
            rms_values.append(rms)
            dynamic_threshold = max(self.vad_min_rms, state.noise_rms * self.vad_noise_multiplier)
            if rms >= dynamic_threshold:
                speech_votes += 1
            else:
                state.noise_rms = (state.noise_rms * 0.95) + (rms * 0.05)
            total_votes += 1
        speech_ratio = (speech_votes / total_votes) if total_votes > 0 else 0.0
        chunk_is_speech = speech_ratio >= 0.5

        speech_started = False
        speech_ended = False
        if chunk_is_speech:
            state.consecutive_speech_frames += 1
            state.consecutive_silence_frames = 0
            if not state.speech_active and state.consecutive_speech_frames >= self.vad_start_frames:
                state.speech_active = True
                state.speech_started_once = True
                speech_started = True
        else:
            state.consecutive_speech_frames = 0
            state.consecutive_silence_frames += 1
            if state.speech_active and state.consecutive_silence_frames >= self.vad_end_frames:
                state.speech_active = False
                speech_ended = True

        avg_rms = float(np.mean(rms_values)) if rms_values else 0.0
        threshold = max(self.vad_min_rms, state.noise_rms * self.vad_noise_multiplier)
        self.vad_history.append({"rms": avg_rms, "threshold": threshold, "speech": chunk_is_speech})
        return {
            "speech_started": speech_started,
            "speech_ended": speech_ended,
            "speech_active": state.speech_active,
            "vad_available": state.available,
            "rms": avg_rms,
            "threshold": threshold,
            "speech_ratio": speech_ratio,
        }

    def _transcribe_pcm_bytes(self, audio_bytes: bytes, sample_rate_hz: int, language: str | None, beam_size: int) -> dict:
        audio = self._pcm_s16le_to_float32_mono(audio_bytes)
        if audio.size == 0:
            return {"text": "", "segments": []}
        segments, info = self.model.transcribe(
            audio,
            language=language if language else None,
            vad_filter=True,
            beam_size=beam_size,
            condition_on_previous_text=False,
        )
        segment_list = []
        text_parts: list[str] = []
        for segment in segments:
            text_parts.append(segment.text.strip())
            segment_list.append(
                {
                    "start_s": segment.start,
                    "end_s": segment.end,
                    "text": segment.text.strip(),
                    "avg_logprob": getattr(segment, "avg_logprob", None),
                    "no_speech_prob": getattr(segment, "no_speech_prob", None),
                }
            )
        return {
            "text": " ".join(part for part in text_parts if part).strip(),
            "segments": segment_list,
            "detected_language": info.language,
            "language_probability": info.language_probability,
            "sample_rate_hz": sample_rate_hz,
        }

    def transcribe(self, payload: dict) -> dict:
        audio_base64 = str(payload.get("audio_base64", ""))
        if not audio_base64:
            raise ValueError("audio_base64_required")

        audio_format = str(payload.get("audio_format", "wav")).lower()
        language = payload.get("language")
        beam_size = int(payload.get("beam_size", 1))

        audio_bytes = self._decode_audio_bytes(audio_base64)
        if audio_format == "pcm_s16le":
            sample_rate_hz = int(payload.get("sample_rate_hz", 16000))
            audio = self._pcm_s16le_to_float32_mono(audio_bytes)
        elif audio_format == "wav":
            audio, sample_rate_hz = self._wav_bytes_to_float32_mono(audio_bytes)
        else:
            raise ValueError("unsupported_audio_format")

        if audio.size == 0:
            return {"text": "", "segments": [], "is_final": bool(payload.get("is_final", True))}

        result = self._transcribe_pcm_bytes(
            (audio * 32768.0).astype(np.int16).tobytes(),
            sample_rate_hz,
            language=language,
            beam_size=beam_size,
        )
        result["is_final"] = bool(payload.get("is_final", True))
        return result

    def start_session(self, payload: dict) -> dict:
        sample_rate_hz = int(payload.get("sample_rate_hz", 16000))
        session_id = str(uuid.uuid4())
        self.sessions[session_id] = BufferedAudioSession(sample_rate_hz=sample_rate_hz, vad_state=SimpleVadState())
        return {"session_id": session_id, "sample_rate_hz": sample_rate_hz, "vad_available": True}

    def append_chunk(self, payload: dict) -> dict:
        session_id = str(payload.get("session_id", ""))
        if session_id not in self.sessions:
            raise ValueError("unknown_session_id")
        chunk_b64 = str(payload.get("audio_chunk_base64", ""))
        if not chunk_b64:
            raise ValueError("audio_chunk_base64_required")
        chunk = self._decode_audio_bytes(chunk_b64)
        session = self.sessions[session_id]
        session.audio_bytes.extend(chunk)
        if session.vad_state is None:
            session.vad_state = SimpleVadState()
        vad = self._vad_from_pcm_chunk(session.vad_state, session.sample_rate_hz, chunk)

        if vad.get("speech_active", False) or vad.get("speech_started", False):
            session.utterance_audio_bytes.extend(chunk)
        elif session.vad_state.speech_started_once and not vad.get("vad_available", True):
            # Fallback path: VAD failed after speech started, keep buffering and let manual interruption handle stop.
            session.utterance_audio_bytes.extend(chunk)

        utterance_text = ""
        is_final = False
        if vad.get("speech_ended", False) and session.utterance_audio_bytes:
            language = payload.get("language")
            beam_size = int(payload.get("beam_size", 1))
            result = self._transcribe_pcm_bytes(bytes(session.utterance_audio_bytes), session.sample_rate_hz, language, beam_size)
            utterance_text = result.get("text", "")
            if utterance_text:
                session.utterance_texts.append(utterance_text)
            session.utterance_audio_bytes.clear()
            is_final = bool(utterance_text)

        return {
            "session_id": session_id,
            "buffered_bytes": len(session.audio_bytes),
            "is_final": is_final,
            "text": utterance_text,
            "speech_started": bool(vad.get("speech_started", False)),
            "speech_active": bool(vad.get("speech_active", False)),
            "vad_available": bool(vad.get("vad_available", True)),
            "manual_interrupt_only": not bool(vad.get("vad_available", True)),
            "rms": vad.get("rms", 0.0),
            "threshold": vad.get("threshold", 0.0),
        }

    def finalize_session(self, payload: dict) -> dict:
        session_id = str(payload.get("session_id", ""))
        if session_id not in self.sessions:
            raise ValueError("unknown_session_id")
        buffered = self.sessions.pop(session_id)
        language = payload.get("language")
        beam_size = int(payload.get("beam_size", 1))
        if buffered.utterance_audio_bytes:
            result = self._transcribe_pcm_bytes(bytes(buffered.utterance_audio_bytes), buffered.sample_rate_hz, language, beam_size)
            final_text = result.get("text", "")
            if final_text:
                buffered.utterance_texts.append(final_text)
        else:
            result = self._transcribe_pcm_bytes(bytes(buffered.audio_bytes), buffered.sample_rate_hz, language, beam_size)
        result["is_final"] = True
        result["utterances"] = buffered.utterance_texts
        if buffered.utterance_texts:
            result["text"] = " ".join(buffered.utterance_texts).strip()
        result["session_id"] = session_id
        result["vad_available"] = buffered.vad_state.available if buffered.vad_state is not None else False
        return result


SPEECH_ADAPTER = FasterWhisperSpeechAdapter()


class MediaPipeGestureAdapter:
    """Hand Landmarker + tiny fixed gesture vocabulary."""

    SUPPORTED_LABELS = {"next", "repeat", "option_a", "option_b", "none"}

    def __init__(self) -> None:
        self.model_path = DEFAULT_GESTURE_MODEL_PATH
        self.failure_reason = ""
        self.landmarker: vision.HandLandmarker | None = None
        self._init_landmarker()

    def _init_landmarker(self) -> None:
        if not os.path.exists(self.model_path):
            self.failure_reason = "hand_landmarker_model_missing"
            logging.warning("Gesture adapter disabled: model not found at %s", self.model_path)
            return
        try:
            options = vision.HandLandmarkerOptions(
                base_options=mp_python.BaseOptions(model_asset_path=self.model_path),
                num_hands=1,
                min_hand_detection_confidence=0.4,
                min_hand_presence_confidence=0.4,
                min_tracking_confidence=0.4,
            )
            self.landmarker = vision.HandLandmarker.create_from_options(options)
            logging.info("Gesture adapter ready with MediaPipe model: %s", self.model_path)
        except Exception as exc:  # noqa: BLE001
            self.failure_reason = "hand_landmarker_init_failed"
            self.landmarker = None
            logging.warning("Gesture adapter failed to initialize: %s", exc)

    @staticmethod
    def _decode_image(payload: dict) -> np.ndarray:
        image_base64 = str(payload.get("image_base64", ""))
        if not image_base64:
            raise ValueError("image_base64_required")
        try:
            image_bytes = base64.b64decode(image_base64)
        except Exception as exc:  # noqa: BLE001
            raise ValueError("invalid_image_base64") from exc
        image = cv2.imdecode(np.frombuffer(image_bytes, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None or image.size == 0:
            raise ValueError("invalid_image_bytes")
        return image

    @staticmethod
    def _distance(a, b) -> float:
        dx = float(a.x - b.x)
        dy = float(a.y - b.y)
        return float(np.sqrt(dx * dx + dy * dy))

    @staticmethod
    def _is_finger_extended(landmarks: list, tip_idx: int, pip_idx: int) -> bool:
        return float(landmarks[tip_idx].y) < float(landmarks[pip_idx].y)

    @staticmethod
    def _is_thumb_extended(landmarks: list, handedness_name: str) -> bool:
        thumb_tip = landmarks[4]
        thumb_ip = landmarks[3]
        if handedness_name.lower() == "left":
            return float(thumb_tip.x) > float(thumb_ip.x)
        return float(thumb_tip.x) < float(thumb_ip.x)

    def _map_landmarks_to_label(self, landmarks: list, handedness_name: str, detection_score: float) -> dict:
        thumb_extended = self._is_thumb_extended(landmarks, handedness_name)
        index_extended = self._is_finger_extended(landmarks, 8, 6)
        middle_extended = self._is_finger_extended(landmarks, 12, 10)
        ring_extended = self._is_finger_extended(landmarks, 16, 14)
        pinky_extended = self._is_finger_extended(landmarks, 20, 18)
        extended_count = sum([thumb_extended, index_extended, middle_extended, ring_extended, pinky_extended])

        pinch_distance = self._distance(landmarks[4], landmarks[8])
        wrist_middle_distance = max(self._distance(landmarks[0], landmarks[12]), 0.01)
        pinch_ratio = pinch_distance / wrist_middle_distance

        label = "none"
        heuristic_confidence = 0.15
        if extended_count >= 4:
            label = "next"
            heuristic_confidence = 0.9
        elif index_extended and middle_extended and not ring_extended and not pinky_extended:
            label = "option_b"
            heuristic_confidence = 0.82
        elif index_extended and not middle_extended and not ring_extended and not pinky_extended:
            label = "option_a"
            heuristic_confidence = 0.78
        elif pinch_ratio < 0.35:
            label = "repeat"
            heuristic_confidence = 0.75

        confidence = max(0.0, min(1.0, (0.55 * detection_score) + (0.45 * heuristic_confidence)))
        return {
            "label": label,
            "confidence": confidence,
            "details": {
                "extended_count": extended_count,
                "pinch_ratio": pinch_ratio,
                "handedness": handedness_name,
            },
        }

    def detect(self, payload: dict) -> dict:
        if self.landmarker is None:
            return {
                "gesture": {"label": "none", "confidence": 0.0},
                "landmarker_available": False,
                "error": self.failure_reason or "hand_landmarker_unavailable",
            }

        image = self._decode_image(payload)
        image_rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=image_rgb)
        result = self.landmarker.detect(mp_image)

        if not result.hand_landmarks:
            return {"gesture": {"label": "none", "confidence": 0.0}, "landmarker_available": True, "hands_detected": 0}

        landmarks = result.hand_landmarks[0]
        handedness_name = "unknown"
        detection_score = 0.0
        if result.handedness and result.handedness[0]:
            handedness = result.handedness[0][0]
            handedness_name = str(getattr(handedness, "category_name", "unknown") or "unknown")
            detection_score = float(getattr(handedness, "score", 0.0) or 0.0)
        mapped = self._map_landmarks_to_label(landmarks, handedness_name, detection_score)
        if mapped["label"] not in self.SUPPORTED_LABELS:
            mapped["label"] = "none"
            mapped["confidence"] = 0.0
        return {
            "gesture": {"label": mapped["label"], "confidence": mapped["confidence"]},
            "landmarker_available": True,
            "hands_detected": len(result.hand_landmarks),
            "details": mapped["details"],
        }


GESTURE_ADAPTER = MediaPipeGestureAdapter()


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
    branch_id = str(turn_context.get("branch_id", "") or "").strip().lower()
    gesture_present = gesture_label in {"next", "repeat", "option_a", "option_b"} and gesture_confidence > 0.0

    if gesture_present and gesture_label == "option_a":
        assistant_text = "Gesture selected rice cooker branch."
        advance_step = False
        target = "branch_rice_cooker"
        new_branch_id = "rice_cooker"
    elif gesture_present and gesture_label == "option_b":
        assistant_text = "Gesture selected pot branch."
        advance_step = False
        target = "branch_pot"
        new_branch_id = "pot"
    elif "rice cooker" in utterance:
        assistant_text = "Okay, using the rice cooker branch."
        advance_step = False
        target = "branch_rice_cooker"
        new_branch_id = "rice_cooker"
    elif "pot" in utterance:
        assistant_text = "Okay, using the pot branch."
        advance_step = False
        target = "branch_pot"
        new_branch_id = "pot"
    elif gesture_present and gesture_label == "next":
        if next_step_instruction:
            if branch_id:
                assistant_text = f"Gesture next ({branch_id}): {next_step_instruction}"
            else:
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
            f"Gesture repeat ({branch_id}): {current_step_instruction}"
            if branch_id and current_step_instruction
            else f"Gesture repeat: {current_step_instruction}"
            if current_step_instruction
            else "Gesture repeat received, but current step is unavailable."
        )
        advance_step = False
        target = "current_step"
        new_branch_id = None
    elif "current step" in utterance:
        assistant_text = (
            f"Current step ({branch_id}): {current_step_instruction}"
            if branch_id and current_step_instruction
            else f"Current step: {current_step_instruction}"
            if current_step_instruction
            else "Current step is not available."
        )
        advance_step = False
        target = "current_step"
        new_branch_id = None
    elif "what next" in utterance or gesture_label == "next":
        if next_step_instruction:
            if branch_id:
                assistant_text = f"Next instruction ({branch_id}): {next_step_instruction}"
            else:
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
                    "stt_model": SPEECH_ADAPTER.model_name,
                    "stt_device": SPEECH_ADAPTER.device,
                    "stt_compute_type": SPEECH_ADAPTER.compute_type,
                    "gesture_landmarker_available": GESTURE_ADAPTER.landmarker is not None,
                    "gesture_model_path": GESTURE_ADAPTER.model_path,
                },
            )
            return

        self._write_json(404, {"error": "not_found", "path": self.path})

    def do_POST(self) -> None:
        logging.info("request POST %s", self.path)
        content_length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(content_length) if content_length > 0 else b"{}"

        try:
            payload = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            self._write_json(400, {"error": "invalid_json"})
            return

        if self.path == "/plan":
            logging.info("serialized TurnContext request: %s", json.dumps(payload, separators=(",", ":")))
            logging.info("planner received user_utterance: %s", payload.get("user_utterance", ""))
            planner_response = _deterministic_mock_planner(payload)
            if planner_response.get("new_branch_id"):
                logging.info("planner branch selection: %s", planner_response.get("new_branch_id"))
            logging.info("serialized PlannerResponse response: %s", json.dumps(planner_response, separators=(",", ":")))
            self._write_json(200, planner_response)
            return

        try:
            if self.path == "/stt/transcribe":
                result = SPEECH_ADAPTER.transcribe(payload)
                logging.info("stt transcribe complete: chars=%d final=%s", len(result.get("text", "")), result.get("is_final"))
                self._write_json(200, result)
                return
            if self.path == "/stt/session/start":
                self._write_json(200, SPEECH_ADAPTER.start_session(payload))
                return
            if self.path == "/stt/session/chunk":
                self._write_json(200, SPEECH_ADAPTER.append_chunk(payload))
                return
            if self.path == "/stt/session/finalize":
                result = SPEECH_ADAPTER.finalize_session(payload)
                logging.info("stt finalize complete: session_id=%s chars=%d", result.get("session_id"), len(result.get("text", "")))
                self._write_json(200, result)
                return
            if self.path == "/gesture/detect":
                result = GESTURE_ADAPTER.detect(payload)
                gesture = result.get("gesture", {})
                logging.info(
                    "gesture detect complete: label=%s confidence=%.2f hands=%s",
                    gesture.get("label", "none"),
                    float(gesture.get("confidence", 0.0) or 0.0),
                    result.get("hands_detected", 0),
                )
                self._write_json(200, result)
                return
        except ValueError as exc:
            self._write_json(400, {"error": str(exc)})
            return
        except Exception as exc:  # noqa: BLE001
            logging.exception("sidecar endpoint failure: %s", exc)
            self._write_json(500, {"error": "sidecar_failure", "detail": str(exc)})
            return

        self._write_json(404, {"error": "not_found", "path": self.path})


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
