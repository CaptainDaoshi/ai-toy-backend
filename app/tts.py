import io
import wave

import edge_tts
import miniaudio


MAX_MP3_BYTES = 2 * 1024 * 1024
MAX_WAV_BYTES = 4 * 1024 * 1024


class SpeechSynthesisUnavailable(RuntimeError):
    pass


async def synthesize_speech_wav(
    text: str,
    voice_name: str = "zh-CN-XiaoxiaoNeural",
    rate: str = "+0%",
) -> bytes:
    """Synthesize speech online, then return a 16-bit mono PCM WAV."""
    try:
        mp3_data = bytearray()
        communicate = edge_tts.Communicate(text=text, voice=voice_name, rate=rate)
        async for chunk in communicate.stream():
            if chunk["type"] != "audio":
                continue
            mp3_data.extend(chunk["data"])
            if len(mp3_data) > MAX_MP3_BYTES:
                raise SpeechSynthesisUnavailable("Speech audio is too large")

        if not mp3_data:
            raise SpeechSynthesisUnavailable("Speech provider returned no audio")

        decoded = miniaudio.mp3_read_s16(bytes(mp3_data))
        if decoded.nchannels != 1 or decoded.sample_width != 2:
            raise SpeechSynthesisUnavailable("Speech provider returned an unsupported audio format")

        output = io.BytesIO()
        with wave.open(output, "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(decoded.sample_rate)
            wav_file.writeframes(decoded.samples.tobytes())
        wav_data = output.getvalue()
        if len(wav_data) > MAX_WAV_BYTES:
            raise SpeechSynthesisUnavailable("Speech WAV is too large")
        return wav_data
    except SpeechSynthesisUnavailable:
        raise
    except Exception as error:
        raise SpeechSynthesisUnavailable("Online speech synthesis is unavailable") from error
