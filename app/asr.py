import io
import json
import os
import re
import threading
import wave
from typing import List

try:
    import vosk
except ImportError:  # Keep the rest of the API importable before ASR is installed.
    vosk = None


PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if os.name == "nt" and os.getenv("LOCALAPPDATA"):
    DEFAULT_VOSK_MODEL_PATH = os.path.join(
        os.environ["LOCALAPPDATA"], "ai-toy-vosk", "vosk-model-small-cn-0.22"
    )
else:
    DEFAULT_VOSK_MODEL_PATH = os.path.join(
        PROJECT_DIR, "models", "vosk-model-small-cn-0.22"
    )

_model = None
_model_lock = threading.Lock()


class SpeechRecognitionUnavailable(RuntimeError):
    pass


class InvalidSpeechAudio(ValueError):
    pass


def _load_model():
    global _model
    if _model is not None:
        return _model
    if vosk is None:
        raise SpeechRecognitionUnavailable("vosk is not installed")
    model_path = os.path.expandvars(
        os.getenv("VOSK_MODEL_PATH", DEFAULT_VOSK_MODEL_PATH)
    )
    if not os.path.isabs(model_path):
        model_path = os.path.join(PROJECT_DIR, model_path)
    if not os.path.isdir(model_path):
        raise SpeechRecognitionUnavailable("Vosk Chinese model directory was not found")

    with _model_lock:
        if _model is None:
            try:
                vosk.SetLogLevel(-1)
                _model = vosk.Model(model_path)
            except Exception as error:
                raise SpeechRecognitionUnavailable("Vosk model could not be loaded") from error
    return _model


def _result_text(result_json: str) -> str:
    try:
        payload = json.loads(result_json)
    except (TypeError, ValueError):
        return ""
    text = payload.get("text") if isinstance(payload, dict) else None
    return text.strip() if isinstance(text, str) else ""


def _normalize_transcript(parts: List[str]) -> str:
    text = " ".join(part for part in parts if part).strip()
    text = re.sub(r"\s+", " ", text)
    # Vosk commonly separates Chinese characters with spaces. Preserve spaces
    # between Latin words but join adjacent CJK characters into natural text.
    text = re.sub(r"(?<=[\u3400-\u9fff])\s+(?=[\u3400-\u9fff])", "", text)
    return text


def transcribe_wav(wav_data: bytes) -> str:
    """Recognize a complete 16 kHz, mono, signed-16-bit PCM WAV."""
    try:
        wav_file = wave.open(io.BytesIO(wav_data), "rb")
    except (EOFError, wave.Error) as error:
        raise InvalidSpeechAudio("request body is not a valid PCM WAV") from error

    with wav_file:
        if wav_file.getnchannels() != 1:
            raise InvalidSpeechAudio("WAV must contain exactly one channel")
        if wav_file.getsampwidth() != 2:
            raise InvalidSpeechAudio("WAV samples must be signed 16-bit PCM")
        if wav_file.getframerate() != 16000:
            raise InvalidSpeechAudio("WAV sample rate must be 16000 Hz")
        if wav_file.getcomptype() != "NONE":
            raise InvalidSpeechAudio("WAV must be uncompressed PCM")

        model = _load_model()
        try:
            recognizer = vosk.KaldiRecognizer(model, wav_file.getframerate())
            parts = []
            while True:
                chunk = wav_file.readframes(4000)
                if not chunk:
                    break
                if recognizer.AcceptWaveform(chunk):
                    text = _result_text(recognizer.Result())
                    if text:
                        parts.append(text)
            final_text = _result_text(recognizer.FinalResult())
            if final_text:
                parts.append(final_text)
        except Exception as error:
            raise SpeechRecognitionUnavailable("Vosk recognition failed") from error

    return _normalize_transcript(parts)
