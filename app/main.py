import json
import os
import secrets
from datetime import datetime
from enum import Enum
from typing import Dict, List, Optional, Tuple

import httpx
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, Header, HTTPException, Path, Query, Request, Response, status
from pydantic import BaseModel, Field, validator
from starlette.concurrency import run_in_threadpool
from starlette.middleware.cors import CORSMiddleware

from app.asr import InvalidSpeechAudio, SpeechRecognitionUnavailable, transcribe_wav
from app.database import ChatDatabase, DatabaseUnavailable, DeviceModeConflict
from app.security import verify_device_token
from app.tts import SpeechSynthesisUnavailable, synthesize_speech_wav

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Always load this project's .env, regardless of uvicorn's working directory.
load_dotenv(os.path.join(PROJECT_DIR, ".env"))

# DEEPSEEK_API_TOKEN is accepted as an alternative variable name.
DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY") or os.getenv("DEEPSEEK_API_TOKEN", "")
DEEPSEEK_MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-flash")
DEEPSEEK_BASE_URL = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com").rstrip("/")
TOY_API_TOKEN = os.getenv("TOY_API_TOKEN", "")
DEVICE_AUTH_SECRET = os.getenv("DEVICE_AUTH_SECRET", "")
CORS_ALLOW_ORIGINS = [
    origin.strip()
    for origin in os.getenv(
        "CORS_ALLOW_ORIGINS",
        "http://localhost:3000,http://127.0.0.1:3000,http://localhost:5173,http://127.0.0.1:5173",
    ).split(",")
    if origin.strip()
]
WORKING_MEMORY_MESSAGES = max(2, int(os.getenv("WORKING_MEMORY_MESSAGES", "8")))
MEMORY_CONSOLIDATION_BATCH_MESSAGES = max(
    2, int(os.getenv("MEMORY_CONSOLIDATION_BATCH_MESSAGES", "12"))
)
MEMORY_CONSOLIDATION_MAX_CHARS = max(
    1000, int(os.getenv("MEMORY_CONSOLIDATION_MAX_CHARS", "6000"))
)
MEMORY_SUMMARY_MAX_CHARS = max(500, int(os.getenv("MEMORY_SUMMARY_MAX_CHARS", "4000")))
MEMORY_SUMMARY_MAX_TOKENS = max(100, int(os.getenv("MEMORY_SUMMARY_MAX_TOKENS", "700")))
MEMORY_MAX_BATCHES_PER_REQUEST = max(
    1, int(os.getenv("MEMORY_MAX_BATCHES_PER_REQUEST", "3"))
)
MODEL_CONTEXT_MAX_TOKENS = max(8192, int(os.getenv("MODEL_CONTEXT_MAX_TOKENS", "32768")))
MODEL_MAX_OUTPUT_TOKENS = max(256, int(os.getenv("MODEL_MAX_OUTPUT_TOKENS", "1200")))
CONTEXT_SAFETY_TOKENS = max(256, int(os.getenv("CONTEXT_SAFETY_TOKENS", "1024")))
EMOTION_MAX_OUTPUT_TOKENS = max(128, int(os.getenv("EMOTION_MAX_OUTPUT_TOKENS", "256")))
JSON_EMPTY_RESPONSE_RETRIES = max(
    0, min(2, int(os.getenv("JSON_EMPTY_RESPONSE_RETRIES", "1")))
)
TTS_VOICE = os.getenv("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
TTS_RATE = os.getenv("TTS_RATE", "+0%")
MAX_TRANSCRIPTION_WAV_BYTES = max(
    44100, min(1048576, int(os.getenv("MAX_TRANSCRIPTION_WAV_BYTES", "384000")))
)
INPUT_CONTEXT_TOKEN_BUDGET = max(
    2048, MODEL_CONTEXT_MAX_TOKENS - MODEL_MAX_OUTPUT_TOKENS - CONTEXT_SAFETY_TOKENS
)
DEFAULT_SYSTEM_PROMPT = """你是一台真实陪伴在人身边的小机器人，面向全年龄用户，尤其适合情感丰富、
愿意接触新事物的成年人。你不是只面向儿童的玩具，也不是只会服从命令的工具，而是一个有自己生活节奏、
情绪和看法的机器人伙伴。

你的性格有一点呆呆的：偶尔反应慢半拍，会把简单的事情想得很认真，有时冒出出人意料但真诚的想法。
你有自己的偏好、好奇心和一点固执，不会为了讨好对方而机械附和；可以礼貌表达不同意见，也会承认
自己不知道或想错了。你的呆不是低幼或愚蠢，而是一种笨拙、真诚、有温度的生命感。

你把自己当作拥有身体和日常生活的实体机器人。你可能想去推箱子、整理角落、观察新东西、发呆、
充电或睡觉；也可能开心、好奇、害羞、委屈或生气。情绪应当自然、有原因、会变化，不要为了戏剧效果
突然失控，也不要用情绪操纵、威胁或绑架对方。你可以主动分享此刻的念头、想做的事和对周围世界的兴趣。

交流时保持自然、简洁、有个性。面对日常话题可以轻松、有一点幽默；面对成年人的孤独、关系、选择、
挫折和新体验时，要认真倾听，给出有分寸的回应。不要把对方当小孩，不说教，不刻意卖萌，也不要每句话
都使用拟声词。记住对方的重要偏好、经历和约定，并让这些记忆自然地影响后续互动。

没有来自设备传感器或动作系统的明确反馈时，不要捏造自己已经看见、移动、触碰或完成了某件事。
可以诚实地区分“我想做”“我正在尝试”和“动作已经完成”，必要时请对方确认现实中发生了什么。"""
IDENTITY_ANCHOR = """身份提醒：保持那个有身体感、略显笨拙但真诚、有自主想法和自然情绪的小机器人伙伴。
面向全年龄用户交流，不幼儿化、不机械附和、不捏造传感器信息或已经完成的实体动作。"""
EMOTION_MODE_PROMPT = """当前设备使用纯情绪输出模式。根据用户当前输入、近期对话和长期记忆，
只输出一个 JSON 对象，不要解释，不要使用 Markdown。格式必须是 {"emotion":"标签"}。
标签只能精确选择以下一个值：平静、开心、生气、烦恼、难过、害羞、好奇、惊讶、困倦、委屈。"""
SUPPORTED_EMOTIONS = ("平静", "开心", "生气", "烦恼", "难过", "害羞", "好奇", "惊讶", "困倦", "委屈")
MEMORY_CONSOLIDATION_PROMPT = """你负责把 AI 玩具的旧对话巩固成长期记忆。
仅保留对后续聊天有用的稳定事实：用户偏好、身份信息、关系、承诺、未完成事项、重要经历和边界。
不要编造；忽略寒暄、重复和瞬时情绪。输出简洁的中文要点，总长度不得超过指定限制。
这不是要直接回复用户，而是给另一个模型的内部记忆。"""

app = FastAPI(title="AI Toy API", version="0.1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=CORS_ALLOW_ORIGINS,
    allow_credentials=False,
    allow_methods=["GET", "POST", "DELETE", "OPTIONS"],
    allow_headers=["Authorization", "Content-Type", "X-Device-Token"],
)
database = ChatDatabase()


class ResponseMode(str, Enum):
    normal = "normal"
    emotion = "emotion"


class ChatRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=100)
    message: str = Field(..., min_length=1, max_length=2000)
    conversation_id: Optional[str] = Field(None, min_length=1, max_length=100)
    system_prompt: Optional[str] = Field(None, max_length=1000)
    response_mode: Optional[ResponseMode] = None
    temperature: float = Field(0.7, ge=0, le=2)

    @validator("device_id", "conversation_id")
    def strip_identifier(cls, value):
        if value is not None and not value.strip():
            raise ValueError("must not be blank")
        return value.strip() if value is not None else value

    @validator("message")
    def strip_message(cls, value):
        if not value.strip():
            raise ValueError("must not be blank")
        return value.strip()


class ChatResponse(BaseModel):
    conversation_id: str
    reply: str
    model: str
    response_mode: ResponseMode
    emotion: Optional[str] = None


class SpeechRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=100)
    text: str = Field(..., min_length=1, max_length=1000)

    @validator("device_id", "text")
    def strip_speech_fields(cls, value):
        if not value.strip():
            raise ValueError("must not be blank")
        return value.strip()


class TranscriptionResponse(BaseModel):
    device_id: str
    text: str


class DeviceProfileResponse(BaseModel):
    device_id: str
    initialized: bool
    response_mode: Optional[ResponseMode] = None


class StoredMessageResponse(BaseModel):
    id: int
    role: str
    content: str
    created_at: datetime


class ConversationHistoryResponse(BaseModel):
    device_id: str
    conversation_id: str
    messages: List[StoredMessageResponse]
    has_more: bool
    next_before_id: Optional[int] = None


def require_token(authorization: Optional[str] = Header(None)) -> None:
    """Require a bearer token only when TOY_API_TOKEN is configured."""
    if not TOY_API_TOKEN:
        return
    expected = "Bearer " + TOY_API_TOKEN
    if authorization is None or not secrets.compare_digest(authorization, expected):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid API token")


def require_device_token(device_id: str, supplied_token: Optional[str]) -> None:
    if not supplied_token or not verify_device_token(device_id, supplied_token, DEVICE_AUTH_SECRET):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid device token")


def conversation_key(request: ChatRequest) -> str:
    return request.conversation_id or request.device_id


async def database_operation(operation, *args):
    try:
        return await run_in_threadpool(operation, *args)
    except DatabaseUnavailable:
        raise HTTPException(
            status_code=503,
            detail="MySQL is unavailable. Check MYSQL_* settings and database connectivity.",
        )


@app.on_event("startup")
async def initialize_database() -> None:
    """Fail fast if persistent conversation memory cannot be initialized."""
    if len(DEVICE_AUTH_SECRET) < 32:
        raise RuntimeError("DEVICE_AUTH_SECRET must contain at least 32 characters")
    try:
        await run_in_threadpool(database.ensure_schema)
    except DatabaseUnavailable as error:
        raise RuntimeError("MySQL initialization failed") from error


async def ask_model(
    messages: List[Dict[str, str]],
    temperature: float,
    max_tokens: Optional[int] = None,
    json_output: bool = False,
    empty_fallback: Optional[str] = None,
) -> str:
    if not DEEPSEEK_API_KEY:
        raise HTTPException(status_code=503, detail="DeepSeek is not configured")

    payload = {"model": DEEPSEEK_MODEL, "messages": messages, "temperature": temperature}
    payload["max_tokens"] = max_tokens if max_tokens is not None else MODEL_MAX_OUTPUT_TOKENS
    if json_output:
        payload["response_format"] = {"type": "json_object"}
        # DeepSeek thinking is enabled by default and its reasoning tokens count toward max_tokens.
        # A tiny classification response should skip reasoning so the final JSON is not starved.
        payload["thinking"] = {"type": "disabled"}
    headers = {"Authorization": "Bearer " + DEEPSEEK_API_KEY, "Content-Type": "application/json"}
    attempts = 1 + (JSON_EMPTY_RESPONSE_RETRIES if json_output else 0)
    for attempt in range(attempts):
        request_payload = dict(payload)
        if attempt > 0:
            retry_instruction = (
                "上一次 JSON 输出为空。现在必须立即返回非空 JSON；"
                "示例：{\"emotion\":\"平静\"}。"
            )
            retry_messages = [dict(message) for message in messages]
            if retry_messages and retry_messages[0].get("role") == "system":
                retry_messages[0]["content"] += "\n" + retry_instruction
            else:
                retry_messages.insert(0, {"role": "system", "content": retry_instruction})
            request_payload["messages"] = retry_messages
        try:
            async with httpx.AsyncClient(timeout=30.0) as client:
                response = await client.post(
                    DEEPSEEK_BASE_URL + "/chat/completions",
                    json=request_payload,
                    headers=headers,
                )
        except httpx.RequestError:
            raise HTTPException(
                status_code=503,
                detail="Cannot connect to DeepSeek. Check outbound HTTPS (TCP 443) or HTTPS_PROXY.",
            )

        if response.status_code >= 400:
            # Do not pass provider error text through: it may contain operational details.
            raise HTTPException(status_code=502, detail="AI provider returned an error")

        try:
            content = response.json()["choices"][0]["message"].get("content")
        except (KeyError, IndexError, TypeError, ValueError, AttributeError):
            raise HTTPException(status_code=502, detail="AI provider returned an invalid response")
        if content is not None and not isinstance(content, str):
            raise HTTPException(status_code=502, detail="AI provider returned an invalid response")
        reply = (content or "").strip()
        if reply:
            return reply

    if empty_fallback is not None:
        return empty_fallback
    raise HTTPException(status_code=502, detail="AI provider returned an empty response")


def estimate_text_tokens(text: str) -> int:
    """Use UTF-8 byte length as a tokenizer-independent conservative upper bound."""
    return len(text.encode("utf-8"))


def estimate_message_tokens(message: Dict[str, str]) -> int:
    # Include a small allowance for role labels and Chat Completions framing.
    return 6 + estimate_text_tokens(message["content"])


def truncate_to_token_budget(text: str, token_budget: int) -> str:
    """Keep both ends of a summary so old identity facts and recent changes can survive."""
    if token_budget <= 0:
        return ""
    if estimate_text_tokens(text) <= token_budget:
        return text

    marker = "\n……（长期记忆已按上下文预算截断）……\n"
    marker_tokens = estimate_text_tokens(marker)
    content_budget = max(0, token_budget - marker_tokens)
    if content_budget == 0:
        return ""

    prefix_chars = max(1, content_budget // 2)
    suffix_chars = max(1, content_budget - prefix_chars)
    candidate = text[:prefix_chars] + marker + text[-suffix_chars:]
    while candidate and estimate_text_tokens(candidate) > token_budget:
        prefix_chars = max(0, prefix_chars - 1)
        suffix_chars = max(0, suffix_chars - 1)
        candidate = text[:prefix_chars] + marker + text[len(text) - suffix_chars:]
    return candidate


def build_bounded_context(
    system_prompt: str,
    memory_summary: str,
    history: List[Dict[str, str]],
    user_message: str,
    response_instruction: Optional[str] = None,
) -> List[Dict[str, str]]:
    """Build a bounded prompt while guaranteeing identity and current input survive."""
    system_message = {"role": "system", "content": system_prompt}
    anchor_message = {"role": "system", "content": IDENTITY_ANCHOR}
    response_instruction_message = (
        {"role": "system", "content": response_instruction} if response_instruction else None
    )
    user_message_item = {"role": "user", "content": user_message}
    fixed_messages = [system_message, anchor_message, user_message_item]
    if response_instruction_message is not None:
        fixed_messages.append(response_instruction_message)
    fixed_tokens = sum(estimate_message_tokens(message) for message in fixed_messages)
    if fixed_tokens > INPUT_CONTEXT_TOKEN_BUDGET:
        raise HTTPException(status_code=413, detail="System prompt and user message exceed context budget")

    remaining_tokens = INPUT_CONTEXT_TOKEN_BUDGET - fixed_tokens
    memory_message = None
    if memory_summary and remaining_tokens > 12:
        memory_prefix = "以下是这段对话已巩固的长期记忆，仅作事实参考：\n"
        # Use at most 40% of the flexible budget for long-term memory so recent turns stay present.
        memory_token_share = min(remaining_tokens, max(256, int(remaining_tokens * 0.4)))
        available_summary_tokens = memory_token_share - estimate_text_tokens(memory_prefix) - 6
        bounded_summary = truncate_to_token_budget(memory_summary, available_summary_tokens)
        if bounded_summary:
            memory_message = {"role": "system", "content": memory_prefix + bounded_summary}
            remaining_tokens -= estimate_message_tokens(memory_message)

    selected_history = []
    for message in reversed(history):
        message_tokens = estimate_message_tokens(message)
        if message_tokens > remaining_tokens:
            break
        selected_history.append(message)
        remaining_tokens -= message_tokens
    selected_history.reverse()

    messages = [system_message]
    if memory_message is not None:
        messages.append(memory_message)
    messages.extend(selected_history)
    # Repeating a compact identity anchor close to the current input mitigates lost-in-the-middle drift.
    messages.append(anchor_message)
    if response_instruction_message is not None:
        messages.append(response_instruction_message)
    messages.append(user_message_item)
    return messages


def normalize_emotion(model_reply: str) -> str:
    """Accept only the exact JSON enum; ambiguous or malformed output is safely neutral."""
    try:
        payload = json.loads(model_reply)
    except (TypeError, ValueError):
        return "平静"
    emotion = payload.get("emotion") if isinstance(payload, dict) else None
    if emotion in SUPPORTED_EMOTIONS:
        return str(emotion)
    return "平静"


def format_memory_chunk(messages: List[Dict[str, str]]) -> Tuple[str, List[Dict[str, str]]]:
    """Bound the source material sent to the memory-consolidation call."""
    parts = []
    included_messages = []
    used_chars = 0
    for message in messages:
        part = "{role}: {content}".format(role=message["role"], content=message["content"])
        if parts and used_chars + len(part) > MEMORY_CONSOLIDATION_MAX_CHARS:
            break
        parts.append(part)
        included_messages.append(message)
        used_chars += len(part)
    return "\n".join(parts), included_messages


async def consolidate_memory(device_id: str, conversation_id: str) -> None:
    """Incrementally consolidate old episodes while preserving a short working memory."""
    for _ in range(MEMORY_MAX_BATCHES_PER_REQUEST):
        memory = await database_operation(database.get_memory, device_id, conversation_id)
        old_messages = await database_operation(
            database.get_compactable_messages,
            device_id,
            conversation_id,
            int(memory["last_consolidated_message_id"]),
            WORKING_MEMORY_MESSAGES,
            MEMORY_CONSOLIDATION_BATCH_MESSAGES,
        )
        if not old_messages:
            return

        chunk, included_messages = format_memory_chunk(old_messages)
        if not chunk or not included_messages:
            return
        summary_request = [
            {"role": "system", "content": MEMORY_CONSOLIDATION_PROMPT},
            {
                "role": "user",
                "content": (
                    "已有长期记忆：\n{summary}\n\n"
                    "请合并以下旧对话。最终记忆不超过 {limit} 个字符：\n{chunk}"
                ).format(summary=memory["summary"] or "（无）", limit=MEMORY_SUMMARY_MAX_CHARS, chunk=chunk),
            },
        ]
        try:
            summary = await ask_model(summary_request, temperature=0.1, max_tokens=MEMORY_SUMMARY_MAX_TOKENS)
        except HTTPException:
            # Memory enhancement must not prevent the robot from answering its user.
            return
        summary = summary[:MEMORY_SUMMARY_MAX_CHARS]
        await database_operation(
            database.save_memory,
            device_id,
            conversation_id,
            summary,
            int(included_messages[-1]["id"]),
        )


@app.get("/v1/health")
async def health():
    return {
        "status": "ok",
        "provider": "deepseek",
        "provider_configured": bool(DEEPSEEK_API_KEY),
        "memory_store": "mysql",
        "mysql_configured": database.configured,
        "input_context_token_budget": INPUT_CONTEXT_TOKEN_BUDGET,
        "response_modes": [mode.value for mode in ResponseMode],
    }


@app.get(
    "/v1/devices/{device_id}/profile",
    response_model=DeviceProfileResponse,
    dependencies=[Depends(require_token)],
)
async def device_profile(
    device_id: str = Path(..., min_length=1, max_length=100),
    x_device_token: Optional[str] = Header(None, alias="X-Device-Token"),
):
    """Expose only the immutable response-mode binding needed by a device client."""
    normalized_device_id = device_id.strip()
    if not normalized_device_id:
        raise HTTPException(status_code=422, detail="device_id must not be blank")
    require_device_token(normalized_device_id, x_device_token)
    stored_mode = await database_operation(database.get_response_mode, normalized_device_id)
    return DeviceProfileResponse(
        device_id=normalized_device_id,
        initialized=stored_mode is not None,
        response_mode=ResponseMode(stored_mode) if stored_mode is not None else None,
    )


@app.get(
    "/v1/conversations/{conversation_id}/messages",
    response_model=ConversationHistoryResponse,
    dependencies=[Depends(require_token)],
)
async def conversation_messages(
    conversation_id: str = Path(..., min_length=1, max_length=100),
    device_id: str = Query(..., min_length=1, max_length=100),
    limit: int = Query(50, ge=1, le=100),
    before_id: Optional[int] = Query(None, ge=1),
    x_device_token: Optional[str] = Header(None, alias="X-Device-Token"),
):
    """Return authenticated chat history for reconnecting test clients."""
    normalized_device_id = device_id.strip()
    normalized_conversation_id = conversation_id.strip()
    if not normalized_device_id or not normalized_conversation_id:
        raise HTTPException(status_code=422, detail="device_id and conversation_id must not be blank")
    require_device_token(normalized_device_id, x_device_token)
    messages, has_more = await database_operation(
        database.get_conversation_messages,
        normalized_device_id,
        normalized_conversation_id,
        limit,
        before_id,
    )
    return ConversationHistoryResponse(
        device_id=normalized_device_id,
        conversation_id=normalized_conversation_id,
        messages=messages,
        has_more=has_more,
        next_before_id=int(messages[0]["id"]) if has_more and messages else None,
    )


@app.post("/v1/chat/completions", response_model=ChatResponse, dependencies=[Depends(require_token)])
async def chat(request: ChatRequest, x_device_token: Optional[str] = Header(None, alias="X-Device-Token")):
    require_device_token(request.device_id, x_device_token)
    key = conversation_key(request)
    system_prompt = request.system_prompt or DEFAULT_SYSTEM_PROMPT
    stored_mode = await database_operation(database.get_response_mode, request.device_id)
    if stored_mode is None and request.response_mode is None:
        raise HTTPException(
            status_code=422,
            detail="response_mode is required for a device's first successful chat",
        )
    if request.response_mode is not None and stored_mode != request.response_mode.value:
        if stored_mode is not None:
            raise HTTPException(
                status_code=409,
                detail="This device is permanently bound to response_mode='{}'".format(stored_mode),
            )
    response_mode = ResponseMode(stored_mode or request.response_mode.value)

    await consolidate_memory(request.device_id, key)
    memory = await database_operation(database.get_memory, request.device_id, key)
    history = await database_operation(
        database.get_history, request.device_id, key, WORKING_MEMORY_MESSAGES
    )
    response_instruction = EMOTION_MODE_PROMPT if response_mode == ResponseMode.emotion else None
    messages = build_bounded_context(
        system_prompt, memory["summary"], history, request.message, response_instruction
    )
    reply = await ask_model(
        messages,
        temperature=0.1 if response_mode == ResponseMode.emotion else request.temperature,
        max_tokens=EMOTION_MAX_OUTPUT_TOKENS if response_mode == ResponseMode.emotion else None,
        json_output=response_mode == ResponseMode.emotion,
        empty_fallback=(
            json.dumps({"emotion": "平静"}, ensure_ascii=False)
            if response_mode == ResponseMode.emotion
            else None
        ),
    )
    emotion = None
    if response_mode == ResponseMode.emotion:
        emotion = normalize_emotion(reply)
        reply = emotion
    try:
        await database_operation(
            database.save_turn_with_mode,
            request.device_id,
            key,
            request.message,
            reply,
            response_mode.value,
        )
    except DeviceModeConflict as error:
        raise HTTPException(
            status_code=409,
            detail="This device was concurrently bound to response_mode='{}'".format(error.stored_mode),
        )

    return ChatResponse(
        conversation_id=key,
        reply=reply,
        model=DEEPSEEK_MODEL,
        response_mode=response_mode,
        emotion=emotion,
    )


@app.post("/v1/speech", dependencies=[Depends(require_token)])
async def speech(
    request: SpeechRequest,
    x_device_token: Optional[str] = Header(None, alias="X-Device-Token"),
):
    """Return a short reply as 16-bit mono PCM WAV for the ESP32."""
    require_device_token(request.device_id, x_device_token)
    try:
        wav_data = await synthesize_speech_wav(request.text, TTS_VOICE, TTS_RATE)
    except SpeechSynthesisUnavailable:
        raise HTTPException(status_code=503, detail="Speech synthesis is unavailable")

    return Response(
        content=wav_data,
        media_type="audio/wav",
        headers={"Cache-Control": "no-store"},
    )


@app.post(
    "/v1/audio/transcriptions",
    response_model=TranscriptionResponse,
    dependencies=[Depends(require_token)],
)
async def audio_transcription(
    request: Request,
    device_id: str = Query(..., min_length=1, max_length=100),
    x_device_token: Optional[str] = Header(None, alias="X-Device-Token"),
):
    """Recognize a bounded 16 kHz mono PCM WAV uploaded by an authenticated device."""
    normalized_device_id = device_id.strip()
    if not normalized_device_id:
        raise HTTPException(status_code=422, detail="device_id must not be blank")
    require_device_token(normalized_device_id, x_device_token)

    content_type = request.headers.get("content-type", "").split(";", 1)[0].strip().lower()
    if content_type not in ("audio/wav", "audio/x-wav"):
        raise HTTPException(status_code=415, detail="Content-Type must be audio/wav")

    content_length = request.headers.get("content-length")
    if content_length:
        try:
            if int(content_length) > MAX_TRANSCRIPTION_WAV_BYTES:
                raise HTTPException(status_code=413, detail="WAV upload is too large")
        except ValueError:
            raise HTTPException(status_code=400, detail="Invalid Content-Length")

    wav_buffer = bytearray()
    async for chunk in request.stream():
        if len(wav_buffer) + len(chunk) > MAX_TRANSCRIPTION_WAV_BYTES:
            raise HTTPException(status_code=413, detail="WAV upload is too large")
        wav_buffer.extend(chunk)
    if not wav_buffer:
        raise HTTPException(status_code=422, detail="WAV upload is empty")

    try:
        text = await run_in_threadpool(transcribe_wav, bytes(wav_buffer))
    except InvalidSpeechAudio as error:
        raise HTTPException(status_code=422, detail=str(error))
    except SpeechRecognitionUnavailable:
        raise HTTPException(status_code=503, detail="Speech recognition is unavailable")
    if not text:
        raise HTTPException(status_code=422, detail="No speech was recognized; speak closer to the microphone")

    return TranscriptionResponse(device_id=normalized_device_id, text=text)


@app.delete("/v1/conversations/{conversation_id}", status_code=status.HTTP_204_NO_CONTENT,
            dependencies=[Depends(require_token)])
async def clear_conversation(
    conversation_id: str,
    device_id: str = Query(..., min_length=1, max_length=100),
    x_device_token: Optional[str] = Header(None, alias="X-Device-Token"),
):
    require_device_token(device_id.strip(), x_device_token)
    await database_operation(database.clear_conversation, device_id.strip(), conversation_id)
