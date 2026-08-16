import asyncio
from array import array
import io
import unittest
from unittest.mock import patch
import wave

from app.asr import InvalidSpeechAudio, transcribe_wav
from app.main import ask_model
from app.tts import SpeechSynthesisUnavailable, synthesize_speech_wav


class FakeResponse:
    status_code = 200

    def __init__(self, content):
        self.content = content

    def json(self):
        return {
            "choices": [
                {
                    "finish_reason": "stop" if self.content else "length",
                    "message": {
                        "content": self.content,
                        "reasoning_content": "internal reasoning" if not self.content else "",
                    },
                }
            ]
        }


class FakeAsyncClient:
    def __init__(self, responses):
        self.responses = list(responses)
        self.payloads = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, traceback):
        return False

    async def post(self, url, json, headers):
        self.payloads.append(json)
        return self.responses.pop(0)


class AskModelEmotionTests(unittest.TestCase):
    def test_json_output_disables_thinking_and_retries_empty_content(self):
        client = FakeAsyncClient(
            [FakeResponse(""), FakeResponse('{"emotion":"开心"}')]
        )
        with patch("app.main.httpx.AsyncClient", return_value=client):
            reply = asyncio.run(
                ask_model(
                    [{"role": "user", "content": "你好"}],
                    temperature=0.1,
                    max_tokens=256,
                    json_output=True,
                    empty_fallback='{\"emotion\":\"平静\"}',
                )
            )

        self.assertEqual(reply, '{"emotion":"开心"}')
        self.assertEqual(len(client.payloads), 2)
        self.assertEqual(client.payloads[0]["thinking"], {"type": "disabled"})
        self.assertIn("上一次 JSON 输出为空", client.payloads[1]["messages"][0]["content"])

    def test_json_output_uses_safe_fallback_after_retry(self):
        client = FakeAsyncClient([FakeResponse(""), FakeResponse("")])
        with patch("app.main.httpx.AsyncClient", return_value=client):
            reply = asyncio.run(
                ask_model(
                    [{"role": "user", "content": "你好"}],
                    temperature=0.1,
                    max_tokens=256,
                    json_output=True,
                    empty_fallback='{\"emotion\":\"平静\"}',
                )
            )

        self.assertEqual(reply, '{"emotion":"平静"}')
        self.assertEqual(len(client.payloads), 2)


class SpeechSynthesisTests(unittest.TestCase):
    @patch("app.tts.miniaudio.mp3_read_s16")
    @patch("app.tts.edge_tts.Communicate")
    def test_returns_pcm_wav(self, communicate, decode):
        class FakeCommunicate:
            async def stream(self):
                yield {"type": "audio", "data": b"fake-mp3"}

        communicate.return_value = FakeCommunicate()
        decode.return_value = type(
            "Decoded",
            (),
            {
                "nchannels": 1,
                "sample_width": 2,
                "sample_rate": 24000,
                "samples": array("h", [0, 100, -100, 0]),
            },
        )()

        wav_data = asyncio.run(synthesize_speech_wav("你好"))

        self.assertEqual(wav_data[:4], b"RIFF")
        self.assertEqual(wav_data[8:12], b"WAVE")

    @patch("app.tts.edge_tts.Communicate")
    def test_rejects_empty_audio(self, communicate):
        class FakeCommunicate:
            async def stream(self):
                if False:
                    yield None

        communicate.return_value = FakeCommunicate()
        with self.assertRaises(SpeechSynthesisUnavailable):
            asyncio.run(synthesize_speech_wav("你好"))


class SpeechRecognitionTests(unittest.TestCase):
    @staticmethod
    def pcm_wav(sample_rate=16000, channels=1, sample_width=2):
        output = io.BytesIO()
        with wave.open(output, "wb") as wav_file:
            wav_file.setnchannels(channels)
            wav_file.setsampwidth(sample_width)
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(b"\x00\x00" * 1600 * channels)
        return output.getvalue()

    def test_transcribes_and_joins_chinese_characters(self):
        class FakeRecognizer:
            def __init__(self, model, sample_rate):
                self.model = model
                self.sample_rate = sample_rate

            def AcceptWaveform(self, chunk):
                return False

            def FinalResult(self):
                return '{"text":"你 是 谁"}'

        fake_vosk = type("FakeVosk", (), {"KaldiRecognizer": FakeRecognizer})
        with patch("app.asr._load_model", return_value=object()), patch("app.asr.vosk", fake_vosk):
            text = transcribe_wav(self.pcm_wav())

        self.assertEqual(text, "你是谁")

    def test_rejects_wrong_sample_rate(self):
        with self.assertRaises(InvalidSpeechAudio):
            transcribe_wav(self.pcm_wav(sample_rate=8000))


if __name__ == "__main__":
    unittest.main()
