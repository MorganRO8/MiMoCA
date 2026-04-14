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
import re
import threading
import uuid
import wave
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from dataclasses import dataclass, field
from pathlib import Path
from urllib import error as urllib_error
from urllib import request as urllib_request

import numpy as np
import cv2
import mediapipe as mp
from faster_whisper import WhisperModel
from ultralytics import YOLO
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

HOST = "127.0.0.1"
PORT = 8080
DEFAULT_LLM_MAX_OUTPUT_CHARS = 220
FIXED_VOCABULARY = ["onion", "knife", "cutting board", "pot", "pan", "bowl", "spoon", "rice cooker"]
DEFAULT_CACHE_ROOT = os.path.join("python", "model_cache")
DEFAULT_STT_CACHE_ROOT = os.path.join(DEFAULT_CACHE_ROOT, "stt")
DEFAULT_VISION_CACHE_ROOT = os.path.join(DEFAULT_CACHE_ROOT, "vision")
DEFAULT_GESTURE_CACHE_ROOT = os.path.join(DEFAULT_CACHE_ROOT, "gesture")


@dataclass
class RuntimeConfig:
    host: str = "127.0.0.1"
    port: int = 8080
    speech_enabled: bool = True
    vision_enabled: bool = True
    gesture_enabled: bool = True
    tts_enabled: bool = True
    stt_model: str = "distil-large-v3"
    stt_device: str = "cpu"
    stt_compute_type: str = "int8"
    gesture_model_path: str = "python/models/hand_landmarker.task"
    gesture_model_url: str = (
        "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task"
    )
    vision_model: str = "yolov8s-worldv2.pt"
    vision_confidence: float = 0.25
    planner_mode: str = "llm"
    planner_provider: str = "openai_compatible"
    llm_base_url: str = "https://api.openai.com/v1"
    llm_model: str = "gpt-4o-mini"
    llm_api_key: str = ""
    llm_timeout_s: float = 12.0
    llm_temperature: float = 0.2
    cache_root: str = DEFAULT_CACHE_ROOT
    stt_cache_root: str = DEFAULT_STT_CACHE_ROOT
    vision_cache_root: str = DEFAULT_VISION_CACHE_ROOT
    gesture_cache_root: str = DEFAULT_GESTURE_CACHE_ROOT
    required_modalities: set[str] = field(default_factory=lambda: {"stt", "vision"})
    allow_degraded_startup: bool = True


def _truthy(raw: str, fallback: bool) -> bool:
    value = (raw or "").strip().lower()
    if not value:
        return fallback
    return value in {"1", "true", "yes", "on"}


def _load_runtime_config() -> RuntimeConfig:
    config = RuntimeConfig()
    config_path = os.getenv("MIMOCA_APP_CONFIG_PATH", "mimoca_app_config.json")
    try:
        payload = json.loads(Path(config_path).read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        payload = {}
    sidecar = payload.get("sidecar", {})
    modalities = payload.get("modalities", {})
    planner = payload.get("planner", {})
    model_paths = payload.get("model_paths", {})

    config.host = str(sidecar.get("host", config.host)).strip() or config.host
    try:
        config.port = int(sidecar.get("port", config.port))
    except Exception:  # noqa: BLE001
        pass
    config.speech_enabled = bool(modalities.get("speech_enabled", config.speech_enabled))
    config.vision_enabled = bool(modalities.get("vision_enabled", config.vision_enabled))
    config.gesture_enabled = bool(modalities.get("gesture_enabled", config.gesture_enabled))
    config.tts_enabled = bool(modalities.get("tts_enabled", config.tts_enabled))
    config.planner_mode = str(planner.get("mode", config.planner_mode)).strip().lower() or config.planner_mode
    config.planner_provider = str(planner.get("provider", config.planner_provider)).strip().lower() or config.planner_provider
    config.llm_base_url = str(planner.get("base_url", config.llm_base_url)).strip() or config.llm_base_url
    config.llm_model = str(planner.get("model", config.llm_model)).strip() or config.llm_model
    config.stt_model = str(model_paths.get("stt_model", config.stt_model)).strip() or config.stt_model
    config.vision_model = str(model_paths.get("vision_model", config.vision_model)).strip() or config.vision_model
    config.gesture_model_path = (
        str(model_paths.get("gesture_model_path", config.gesture_model_path)).strip() or config.gesture_model_path
    )

    config.host = os.getenv("MIMOCA_SIDECAR_HOST", config.host).strip() or config.host
    config.port = int(os.getenv("MIMOCA_SIDECAR_PORT", str(config.port)))
    config.stt_model = os.getenv("MIMOCA_STT_MODEL", config.stt_model).strip() or config.stt_model
    config.stt_device = os.getenv("MIMOCA_STT_DEVICE", config.stt_device).strip() or config.stt_device
    config.stt_compute_type = os.getenv("MIMOCA_STT_COMPUTE_TYPE", config.stt_compute_type).strip() or config.stt_compute_type
    config.gesture_model_path = os.getenv("MIMOCA_GESTURE_MODEL_PATH", config.gesture_model_path).strip() or config.gesture_model_path
    config.gesture_model_url = os.getenv("MIMOCA_GESTURE_MODEL_URL", config.gesture_model_url).strip() or config.gesture_model_url
    config.vision_model = os.getenv("MIMOCA_VISION_MODEL", config.vision_model).strip() or config.vision_model
    config.vision_confidence = float(os.getenv("MIMOCA_VISION_CONFIDENCE", str(config.vision_confidence)))
    config.planner_mode = os.getenv("MIMOCA_PLANNER_MODE", config.planner_mode).strip().lower() or config.planner_mode
    config.planner_provider = os.getenv("MIMOCA_LLM_PROVIDER", config.planner_provider).strip().lower() or config.planner_provider
    config.llm_base_url = os.getenv("MIMOCA_LLM_BASE_URL", config.llm_base_url).strip() or config.llm_base_url
    config.llm_model = os.getenv("MIMOCA_LLM_MODEL", config.llm_model).strip() or config.llm_model
    config.llm_api_key = os.getenv("MIMOCA_LLM_API_KEY", "").strip()
    config.llm_timeout_s = float(os.getenv("MIMOCA_LLM_TIMEOUT_S", str(config.llm_timeout_s)))
    config.llm_temperature = float(os.getenv("MIMOCA_LLM_TEMPERATURE", str(config.llm_temperature)))
    config.cache_root = os.getenv("MIMOCA_MODEL_CACHE_ROOT", config.cache_root)
    config.stt_cache_root = os.getenv("MIMOCA_STT_CACHE_ROOT", os.path.join(config.cache_root, "stt"))
    config.vision_cache_root = os.getenv("MIMOCA_VISION_CACHE_ROOT", os.path.join(config.cache_root, "vision"))
    config.gesture_cache_root = os.getenv("MIMOCA_GESTURE_CACHE_ROOT", os.path.join(config.cache_root, "gesture"))
    required_raw = os.getenv("MIMOCA_REQUIRED_MODALITIES", ",".join(sorted(config.required_modalities)))
    config.required_modalities = {item.strip().lower() for item in required_raw.split(",") if item.strip()}
    config.allow_degraded_startup = _truthy(os.getenv("MIMOCA_ALLOW_DEGRADED_STARTUP", "true"), True)
    return config


CONFIG = _load_runtime_config()


@dataclass
class ModalityReadiness:
    status: str = "downloading"
    progress: float = 0.0
    message: str = "pending"
    error: str = ""


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
        self.model_name = CONFIG.stt_model
        self.device = CONFIG.stt_device
        self.compute_type = CONFIG.stt_compute_type
        self.model: WhisperModel | None = None
        self.load_failure: str = ""
        self.sessions: dict[str, BufferedAudioSession] = {}
        self.vad_frame_ms = int(os.getenv("MIMOCA_VAD_FRAME_MS", "20"))
        self.vad_start_frames = int(os.getenv("MIMOCA_VAD_START_FRAMES", "2"))
        self.vad_end_frames = int(os.getenv("MIMOCA_VAD_END_FRAMES", "15"))
        self.vad_min_rms = float(os.getenv("MIMOCA_VAD_MIN_RMS", "0.008"))
        self.vad_noise_multiplier = float(os.getenv("MIMOCA_VAD_NOISE_MULTIPLIER", "2.8"))
        self.vad_history = deque(maxlen=64)
        self.cache_root = CONFIG.stt_cache_root

    def _load_model_with_fallback(self) -> WhisperModel:
        candidates = [self.model_name]
        if self.model_name == "distil-large-v3":
            candidates.append("large-v3")
        last_error: Exception | None = None
        for candidate in candidates:
            try:
                model = WhisperModel(
                    candidate,
                    device=self.device,
                    compute_type=self.compute_type,
                    download_root=self.cache_root,
                )
                self.model_name = candidate
                logging.info("STT model loaded: %s (device=%s, compute_type=%s)", candidate, self.device, self.compute_type)
                return model
            except Exception as exc:  # noqa: BLE001
                last_error = exc
                logging.warning("STT model load failed for %s: %s", candidate, exc)
        raise RuntimeError(f"Failed to load any STT model candidate: {candidates}") from last_error

    def _ensure_model_loaded(self) -> WhisperModel:
        if self.model is not None:
            return self.model
        try:
            self.model = self._load_model_with_fallback()
            self.load_failure = ""
            return self.model
        except Exception as exc:  # noqa: BLE001
            self.load_failure = str(exc)
            raise RuntimeError("stt_unavailable") from exc

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
        model = self._ensure_model_loaded()
        segments, info = model.transcribe(
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
            "speech_ended": bool(vad.get("speech_ended", False)),
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
        buffered = self.sessions[session_id]
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
        self.sessions.pop(session_id, None)
        return result


SPEECH_ADAPTER = FasterWhisperSpeechAdapter()


class MediaPipeGestureAdapter:
    """Hand Landmarker + tiny fixed gesture vocabulary."""

    SUPPORTED_LABELS = {"next", "repeat", "option_a", "option_b", "none"}

    def __init__(self) -> None:
        self.model_path = CONFIG.gesture_model_path
        self.model_url = CONFIG.gesture_model_url
        self.failure_reason = ""
        self.landmarker: vision.HandLandmarker | None = None
        self._resolved_model_path = self.model_path

    def _init_landmarker(self) -> None:
        if not os.path.exists(self._resolved_model_path):
            self.failure_reason = "hand_landmarker_model_missing"
            logging.warning("Gesture adapter disabled: model not found at %s", self._resolved_model_path)
            return
        try:
            options = vision.HandLandmarkerOptions(
                base_options=mp_python.BaseOptions(model_asset_path=self._resolved_model_path),
                num_hands=1,
                min_hand_detection_confidence=0.4,
                min_hand_presence_confidence=0.4,
                min_tracking_confidence=0.4,
            )
            self.landmarker = vision.HandLandmarker.create_from_options(options)
            logging.info("Gesture adapter ready with MediaPipe model: %s", self._resolved_model_path)
        except Exception as exc:  # noqa: BLE001
            self.failure_reason = "hand_landmarker_init_failed"
            self.landmarker = None
            logging.warning("Gesture adapter failed to initialize: %s", exc)

    def ensure_model(self, progress_callback=None) -> None:
        configured_path = Path(self.model_path)
        configured_path.parent.mkdir(parents=True, exist_ok=True)
        if configured_path.exists():
            self._resolved_model_path = str(configured_path)
            if progress_callback:
                progress_callback(1.0, f"using local model at {configured_path}")
            return

        if not self.model_url:
            raise RuntimeError("gesture_model_url_missing")
        cache_target = Path(CONFIG.gesture_cache_root) / configured_path.name
        cache_target.parent.mkdir(parents=True, exist_ok=True)
        self._download_file_with_progress(self.model_url, cache_target, progress_callback)
        if not configured_path.exists():
            configured_path.write_bytes(cache_target.read_bytes())
        self._resolved_model_path = str(configured_path)

    @staticmethod
    def _download_file_with_progress(url: str, destination: Path, progress_callback=None) -> None:
        with urllib_request.urlopen(url, timeout=60) as response:
            total = int(response.headers.get("Content-Length", "0") or "0")
            read = 0
            with destination.open("wb") as out:
                while True:
                    chunk = response.read(1024 * 256)
                    if not chunk:
                        break
                    out.write(chunk)
                    read += len(chunk)
                    if progress_callback and total > 0:
                        progress_callback(min(1.0, read / total), f"downloaded {read}/{total} bytes")
        if progress_callback:
            progress_callback(1.0, f"downloaded {destination}")

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


class YoloVisionAdapter:
    """Ultralytics YOLO adapter with a tiny fixed cooking vocabulary."""

    def __init__(self) -> None:
        self.model_name = CONFIG.vision_model
        self.confidence = CONFIG.vision_confidence
        self.vocabulary = list(FIXED_VOCABULARY)
        self.model: YOLO | None = None
        self.failure_reason = ""
        self.cache_root = CONFIG.vision_cache_root

    def _init_model(self) -> None:
        try:
            os.environ["ULTRALYTICS_HOME"] = self.cache_root
            self.model = YOLO(self.model_name)
            if "world" in self.model_name.lower():
                self.model.set_classes(self.vocabulary)
            logging.info("Vision adapter ready: model=%s vocabulary=%s", self.model_name, ",".join(self.vocabulary))
        except Exception as exc:  # noqa: BLE001
            self.failure_reason = "vision_model_init_failed"
            self.model = None
            logging.warning("Vision adapter disabled: %s", exc)

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

    def detect(self, payload: dict) -> dict:
        if self.model is None:
            return {"detections": [], "vision_available": False, "error": self.failure_reason or "vision_unavailable"}

        image = self._decode_image(payload)
        conf = float(payload.get("confidence_threshold", self.confidence) or self.confidence)
        results = self.model.predict(source=image, conf=conf, verbose=False)

        detections: list[dict] = []
        for result in results:
            boxes = getattr(result, "boxes", None)
            if boxes is None:
                continue
            for box in boxes:
                cls_idx = int(float(box.cls.item()))
                label = str(result.names.get(cls_idx, "")).strip().lower()
                if label not in self.vocabulary:
                    continue
                conf_score = float(box.conf.item())
                xyxy = box.xyxy[0].tolist()
                detections.append(
                    {
                        "label": label,
                        "confidence": max(0.0, min(1.0, conf_score)),
                        "bbox": [float(xyxy[0]), float(xyxy[1]), float(xyxy[2]), float(xyxy[3])],
                    }
                )

        detections.sort(key=lambda d: d["confidence"], reverse=True)
        logging.info("vision detect complete: count=%d labels=%s", len(detections), [d["label"] for d in detections[:6]])
        return {
            "detections": detections,
            "vision_available": True,
            "model": self.model_name,
            "vocabulary": self.vocabulary,
        }


VISION_ADAPTER = YoloVisionAdapter()


class StartupReadinessManager:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.state = "downloading"
        self.started_at = datetime.now(timezone.utc).isoformat()
        self.updated_at = self.started_at
        self.message = "starting"
        self.required_modalities = set(CONFIG.required_modalities)
        self.allow_degraded = CONFIG.allow_degraded_startup
        self.modalities: dict[str, ModalityReadiness] = {
            "stt": ModalityReadiness(),
            "vision": ModalityReadiness(),
            "gesture": ModalityReadiness(),
        }
        self._thread: threading.Thread | None = None

    def _set_modality(self, name: str, **kwargs) -> None:
        with self._lock:
            current = self.modalities[name]
            for key, value in kwargs.items():
                setattr(current, key, value)
            self.updated_at = datetime.now(timezone.utc).isoformat()

    def _run(self) -> None:
        os.makedirs(CONFIG.cache_root, exist_ok=True)
        self._initialize_stt()
        self._initialize_vision()
        self._initialize_gesture()
        self._finalize_state()

    def _initialize_stt(self) -> None:
        self._set_modality("stt", status="downloading", progress=0.05, message="initializing")
        try:
            SPEECH_ADAPTER._ensure_model_loaded()
            self._set_modality("stt", status="ready", progress=1.0, message="ready", error="")
        except Exception as exc:  # noqa: BLE001
            self._set_modality("stt", status="failed", progress=1.0, message="failed", error=str(exc))

    def _initialize_vision(self) -> None:
        self._set_modality("vision", status="downloading", progress=0.05, message="initializing")
        try:
            VISION_ADAPTER._init_model()
            if VISION_ADAPTER.model is None:
                raise RuntimeError(VISION_ADAPTER.failure_reason or "vision_unavailable")
            self._set_modality("vision", status="ready", progress=1.0, message="ready", error="")
        except Exception as exc:  # noqa: BLE001
            self._set_modality("vision", status="failed", progress=1.0, message="failed", error=str(exc))

    def _initialize_gesture(self) -> None:
        self._set_modality("gesture", status="downloading", progress=0.05, message="ensuring hand model")
        try:
            GESTURE_ADAPTER.ensure_model(progress_callback=lambda p, m: self._set_modality("gesture", progress=p, message=m))
            GESTURE_ADAPTER._init_landmarker()
            if GESTURE_ADAPTER.landmarker is None:
                raise RuntimeError(GESTURE_ADAPTER.failure_reason or "gesture_unavailable")
            self._set_modality("gesture", status="ready", progress=1.0, message="ready", error="")
        except Exception as exc:  # noqa: BLE001
            self._set_modality("gesture", status="failed", progress=1.0, message="failed", error=str(exc))

    def _finalize_state(self) -> None:
        with self._lock:
            required_failures = [
                name
                for name, status in self.modalities.items()
                if name in self.required_modalities and status.status != "ready"
            ]
            if required_failures and not self.allow_degraded:
                self.state = "failed"
                self.message = f"required modality init failed: {','.join(required_failures)}"
            elif required_failures and self.allow_degraded:
                self.state = "degraded"
                self.message = f"degraded startup: {','.join(required_failures)} unavailable"
            else:
                self.state = "ready"
                self.message = "all required modalities ready"
            self.updated_at = datetime.now(timezone.utc).isoformat()

    def ensure_started(self) -> None:
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            if self.state in {"ready", "degraded", "failed"}:
                return
            self._thread = threading.Thread(target=self._run, name="mimoca-startup-init", daemon=True)
            self._thread.start()

    def snapshot(self) -> dict:
        with self._lock:
            avg_progress = 0.0
            if self.modalities:
                avg_progress = sum(item.progress for item in self.modalities.values()) / len(self.modalities)
            return {
                "state": self.state,
                "message": self.message,
                "progress": max(0.0, min(1.0, avg_progress)),
                "required_modalities": sorted(self.required_modalities),
                "allow_degraded_startup": self.allow_degraded,
                "modalities": {
                    name: {
                        "status": modality.status,
                        "progress": modality.progress,
                        "message": modality.message,
                        "error": modality.error,
                    }
                    for name, modality in self.modalities.items()
                },
                "started_at": self.started_at,
                "updated_at": self.updated_at,
                "cache_paths": {
                    "root": CONFIG.cache_root,
                    "stt": CONFIG.stt_cache_root,
                    "vision": CONFIG.vision_cache_root,
                    "gesture": CONFIG.gesture_cache_root,
                },
            }


STARTUP_MANAGER = StartupReadinessManager()


def _constrain_assistant_text(text: str) -> str:
    normalized = re.sub(r"\s+", " ", (text or "").strip())
    if not normalized:
        return "I can help with the current step. Ask what to do next."
    if len(normalized) > DEFAULT_LLM_MAX_OUTPUT_CHARS:
        normalized = normalized[: DEFAULT_LLM_MAX_OUTPUT_CHARS].rstrip(" ,;:")
        if normalized and normalized[-1] not in ".!?":
            normalized += "."
    return normalized


def _coerce_planner_response(candidate: dict) -> dict:
    ui_overlays = candidate.get("ui_overlays", [])
    if not isinstance(ui_overlays, list):
        ui_overlays = []
    return {
        "assistant_text": _constrain_assistant_text(str(candidate.get("assistant_text", ""))),
        "speak": bool(candidate.get("speak", True)),
        "interruptible": bool(candidate.get("interruptible", True)),
        "advance_step": bool(candidate.get("advance_step", False)),
        "new_branch_id": candidate.get("new_branch_id") if candidate.get("new_branch_id") is not None else None,
        "ui_overlays": ui_overlays,
    }


class LlmPlannerAdapter:
    """Provider-agnostic planner boundary with openai-compatible HTTP support."""

    def __init__(self) -> None:
        self.mode = CONFIG.planner_mode if CONFIG.planner_mode in {"mock", "llm"} else "llm"
        self.provider = CONFIG.planner_provider
        self.base_url = CONFIG.llm_base_url.rstrip("/")
        self.model = CONFIG.llm_model
        self.api_key = CONFIG.llm_api_key
        self.timeout_s = CONFIG.llm_timeout_s
        self.temperature = CONFIG.llm_temperature

    @property
    def llm_configured(self) -> bool:
        return bool(self.api_key) and bool(self.model) and self.provider == "openai_compatible"

    @property
    def llm_ready(self) -> bool:
        if self.mode != "llm":
            return False
        return self.llm_configured

    def update_config(self, payload: dict) -> dict:
        mode = str(payload.get("mode", self.mode)).strip().lower()
        if mode not in {"mock", "llm"}:
            raise ValueError("invalid_planner_mode")
        provider = str(payload.get("provider", self.provider)).strip().lower()
        if provider != "openai_compatible":
            raise ValueError("unsupported_llm_provider")
        base_url = str(payload.get("base_url", self.base_url)).strip().rstrip("/")
        model = str(payload.get("model", self.model)).strip()
        api_key = str(payload.get("api_key", self.api_key)).strip()

        self.mode = mode
        self.provider = provider
        self.base_url = base_url or CONFIG.llm_base_url.rstrip("/")
        self.model = model or CONFIG.llm_model
        self.api_key = api_key
        return {
            "mode": self.mode,
            "provider": self.provider,
            "base_url": self.base_url,
            "model": self.model,
            "llm_configured": self.llm_configured,
            "llm_ready": self.llm_ready,
        }

    def _validate_openai_compatible_key(self, api_key: str, timeout_s: float = 6.0) -> dict:
        if not api_key:
            raise ValueError("llm_api_key_missing")
        url = f"{self.base_url}/models"
        req = urllib_request.Request(
            url=url,
            method="GET",
            headers={
                "Authorization": f"Bearer {api_key}",
            },
        )
        try:
            with urllib_request.urlopen(req, timeout=timeout_s) as resp:
                body = resp.read().decode("utf-8")
        except urllib_error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="ignore")
            raise RuntimeError(f"llm_http_error:{exc.code}:{detail[:220]}") from exc
        except Exception as exc:  # noqa: BLE001
            raise RuntimeError(f"llm_request_failed:{exc}") from exc

        parsed = json.loads(body) if body else {}
        data = parsed.get("data", [])
        return {
            "provider": self.provider,
            "base_url": self.base_url,
            "model_count": len(data) if isinstance(data, list) else 0,
        }

    def validate_api_key(self, payload: dict) -> dict:
        api_key = str(payload.get("api_key", "")).strip()
        provider = str(payload.get("provider", self.provider)).strip().lower() or self.provider
        if provider != "openai_compatible":
            raise ValueError("unsupported_llm_provider")
        meta = self._validate_openai_compatible_key(api_key)
        return {
            "ok": True,
            "provider": provider,
            "metadata": meta,
        }

    @staticmethod
    def _compact_context(turn_context: dict) -> dict:
        detections = turn_context.get("detections", [])
        if not isinstance(detections, list):
            detections = []
        compact_detections = []
        for det in detections[:8]:
            if not isinstance(det, dict):
                continue
            compact_detections.append(
                {
                    "label": str(det.get("label", "")).strip(),
                    "confidence": float(det.get("confidence", 0.0) or 0.0),
                }
            )
        return {
            "timestamp": turn_context.get("timestamp"),
            "recipe_id": turn_context.get("recipe_id"),
            "step_id": turn_context.get("step_id"),
            "branch_id": turn_context.get("branch_id"),
            "current_step_instruction": turn_context.get("current_step_instruction"),
            "next_step_instruction": turn_context.get("next_step_instruction"),
            "user_utterance": turn_context.get("user_utterance"),
            "gesture": turn_context.get("gesture", {}),
            "detections": compact_detections,
            "frame_summary": turn_context.get("frame_summary", ""),
            "frame_available": bool(turn_context.get("frame_available", False)),
            "settings": turn_context.get("settings", {}),
        }

    @staticmethod
    def _extract_json_object(text: str) -> dict:
        stripped = text.strip()
        if not stripped:
            raise ValueError("empty_llm_response")
        if stripped.startswith("```"):
            stripped = stripped.strip("`")
            if stripped.lower().startswith("json"):
                stripped = stripped[4:].strip()
        first = stripped.find("{")
        last = stripped.rfind("}")
        if first < 0 or last < 0 or last <= first:
            raise ValueError("llm_response_not_json")
        return json.loads(stripped[first : last + 1])

    def _build_messages(self, turn_context: dict) -> list[dict]:
        system_message = (
            "You are a concise cooking planner for a hands-free assistant. "
            "Return only strict JSON matching PlannerResponse. "
            "assistant_text must be short and spoken-friendly. "
            "Do not claim uncertain detections as facts."
        )
        user_payload = {
            "planner_contract": {
                "assistant_text": "string",
                "speak": "boolean",
                "interruptible": "boolean",
                "advance_step": "boolean",
                "new_branch_id": "string|null",
                "ui_overlays": [{"type": "highlight_label", "target": "string"}],
            },
            "turn_context": self._compact_context(turn_context),
            "rules": {
                "max_assistant_chars": DEFAULT_LLM_MAX_OUTPUT_CHARS,
                "set_advance_step_true_only_when_explicitly_advancing": True,
            },
        }
        return [
            {"role": "system", "content": system_message},
            {"role": "user", "content": json.dumps(user_payload, separators=(",", ":"))},
        ]

    def _invoke_openai_compatible(self, messages: list[dict]) -> dict:
        if not self.api_key:
            raise RuntimeError("llm_api_key_missing")
        url = f"{self.base_url}/chat/completions"
        req_payload = {
            "model": self.model,
            "temperature": self.temperature,
            "response_format": {"type": "json_object"},
            "messages": messages,
        }
        req = urllib_request.Request(
            url=url,
            data=json.dumps(req_payload).encode("utf-8"),
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
            },
        )
        try:
            with urllib_request.urlopen(req, timeout=self.timeout_s) as resp:
                body = resp.read().decode("utf-8")
        except urllib_error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="ignore")
            raise RuntimeError(f"llm_http_error:{exc.code}:{detail[:220]}") from exc
        except Exception as exc:  # noqa: BLE001
            raise RuntimeError(f"llm_request_failed:{exc}") from exc

        parsed = json.loads(body)
        choices = parsed.get("choices", [])
        if not choices:
            raise RuntimeError("llm_empty_choices")
        message = choices[0].get("message", {})
        content = message.get("content", "")
        if isinstance(content, list):
            content = "".join(str(part.get("text", "")) for part in content if isinstance(part, dict))
        if not isinstance(content, str):
            raise RuntimeError("llm_invalid_content")
        logging.info("planner llm raw response: %s", content)
        return self._extract_json_object(content)

    def plan(self, turn_context: dict) -> dict:
        if not self.llm_ready:
            raise RuntimeError("llm_not_ready")
        compact = self._compact_context(turn_context)
        logging.info(
            "planner llm request: provider=%s model=%s payload=%s",
            self.provider,
            self.model,
            json.dumps(compact, separators=(",", ":")),
        )
        messages = self._build_messages(turn_context)
        if self.provider == "openai_compatible":
            return _coerce_planner_response(self._invoke_openai_compatible(messages))
        raise RuntimeError(f"unsupported_llm_provider:{self.provider}")


PLANNER_ADAPTER = LlmPlannerAdapter()
PLANNER_RUNTIME_STATUS = {
    "fallback_active": False,
    "last_error": "",
    "lock": threading.Lock(),
}


def _set_planner_fallback_state(active: bool, error_text: str = "") -> None:
    with PLANNER_RUNTIME_STATUS["lock"]:
        PLANNER_RUNTIME_STATUS["fallback_active"] = active
        PLANNER_RUNTIME_STATUS["last_error"] = (error_text or "")[:220]


def _planner_runtime_snapshot() -> dict:
    with PLANNER_RUNTIME_STATUS["lock"]:
        return {
            "fallback_active": bool(PLANNER_RUNTIME_STATUS["fallback_active"]),
            "last_error": str(PLANNER_RUNTIME_STATUS["last_error"]),
        }


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


def _planner_with_fallback(turn_context: dict) -> dict:
    if PLANNER_ADAPTER.mode != "llm":
        _set_planner_fallback_state(False)
        return _deterministic_mock_planner(turn_context)
    try:
        response = PLANNER_ADAPTER.plan(turn_context)
        logging.info("planner llm structured response: %s", json.dumps(response, separators=(",", ":")))
        _set_planner_fallback_state(False)
        return response
    except Exception as exc:  # noqa: BLE001
        logging.warning("planner llm failed, falling back to mock planner: %s", exc)
        _set_planner_fallback_state(True, str(exc))
        fallback = _deterministic_mock_planner(turn_context)
        fallback["assistant_text"] = _constrain_assistant_text(
            f"I could not verify the planner response, so I am using a conservative fallback. {fallback.get('assistant_text', '')}"
        )
        fallback.setdefault("ui_overlays", []).append({"type": "planner_fallback", "target": "mock"})
        return fallback


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
            STARTUP_MANAGER.ensure_started()
            readiness = STARTUP_MANAGER.snapshot()
            startup_state = readiness.get("state", "downloading")
            ready_for_app = startup_state in {"ready", "degraded"}
            status_code = 200 if ready_for_app else 503
            planner_runtime = _planner_runtime_snapshot()
            self._write_json(
                status_code,
                {
                    "status": "ok" if ready_for_app else "initializing",
                    "service": "mimoca-python-sidecar",
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                    "startup": readiness,
                    "startup_ready": ready_for_app,
                    "startup_summary": readiness.get("message", ""),
                    "stt_model": SPEECH_ADAPTER.model_name,
                    "stt_device": SPEECH_ADAPTER.device,
                    "stt_compute_type": SPEECH_ADAPTER.compute_type,
                    "gesture_landmarker_available": GESTURE_ADAPTER.landmarker is not None,
                    "gesture_model_path": GESTURE_ADAPTER.model_path,
                    "vision_available": VISION_ADAPTER.model is not None,
                    "vision_model": VISION_ADAPTER.model_name,
                    "vision_vocabulary": VISION_ADAPTER.vocabulary,
                    "planner_mode": PLANNER_ADAPTER.mode,
                    "planner_provider": PLANNER_ADAPTER.provider,
                    "planner_llm_configured": PLANNER_ADAPTER.llm_configured,
                    "planner_llm_ready": PLANNER_ADAPTER.llm_ready,
                    "planner_fallback_active": planner_runtime.get("fallback_active", False),
                    "planner_fallback_error": planner_runtime.get("last_error", ""),
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
            planner_response = _planner_with_fallback(payload)
            if planner_response.get("new_branch_id"):
                logging.info("planner branch selection: %s", planner_response.get("new_branch_id"))
            logging.info("serialized PlannerResponse response: %s", json.dumps(planner_response, separators=(",", ":")))
            self._write_json(200, planner_response)
            return

        if self.path == "/planner/configure":
            try:
                updated = PLANNER_ADAPTER.update_config(payload)
            except ValueError as exc:
                self._write_json(400, {"error": str(exc)})
                return
            self._write_json(200, {"ok": True, "planner": updated})
            return

        if self.path == "/planner/validate_key":
            try:
                result = PLANNER_ADAPTER.validate_api_key(payload)
            except ValueError as exc:
                self._write_json(400, {"error": str(exc)})
                return
            except Exception as exc:  # noqa: BLE001
                self._write_json(400, {"ok": False, "error": str(exc)})
                return
            self._write_json(200, result)
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
            if self.path == "/vision/detect":
                result = VISION_ADAPTER.detect(payload)
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
    logging.info(
        "effective config: host=%s port=%d planner_mode=%s planner_provider=%s llm_base_url=%s llm_model=%s "
        "modalities[speech=%s vision=%s gesture=%s tts=%s] model_paths[stt=%s vision=%s gesture=%s] "
        "secrets[llm_api_key=%s]",
        CONFIG.host,
        CONFIG.port,
        CONFIG.planner_mode,
        CONFIG.planner_provider,
        CONFIG.llm_base_url,
        CONFIG.llm_model,
        CONFIG.speech_enabled,
        CONFIG.vision_enabled,
        CONFIG.gesture_enabled,
        CONFIG.tts_enabled,
        CONFIG.stt_model,
        CONFIG.vision_model,
        CONFIG.gesture_model_path,
        "redacted" if CONFIG.llm_api_key else "not_set",
    )
    STARTUP_MANAGER.ensure_started()
    server = ThreadingHTTPServer((CONFIG.host, CONFIG.port), Handler)
    logging.info("service listening on http://%s:%d", CONFIG.host, CONFIG.port)
    server.serve_forever()


if __name__ == "__main__":
    main()
