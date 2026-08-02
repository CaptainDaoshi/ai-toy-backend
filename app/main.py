import os
import secrets
from typing import Dict, List, Optional

import httpx
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, Header, HTTPException, Query, status
from pydantic import BaseModel, Field, validator
from starlette.concurrency import run_in_threadpool

from app.database import ChatDatabase, DatabaseUnavailable

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Always load this project's .env, regardless of uvicorn's working directory.
load_dotenv(os.path.join(PROJECT_DIR, ".env"))

# DEEPSEEK_API_TOKEN is accepted as an alternative variable name.
DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY") or os.getenv("DEEPSEEK_API_TOKEN", "")
DEEPSEEK_MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-flash")
DEEPSEEK_BASE_URL = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com").rstrip("/")
TOY_API_TOKEN = os.getenv("TOY_API_TOKEN", "")
MAX_HISTORY_MESSAGES = max(2, int(os.getenv("MAX_HISTORY_MESSAGES", "20")))
DEFAULT_SYSTEM_PROMPT = "你是一个友善、简洁、适合儿童交流的 AI 玩具伙伴。"

app = FastAPI(title="AI Toy API", version="0.1.0")
database = ChatDatabase()


class ChatRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=100)
    message: str = Field(..., min_length=1, max_length=2000)
    conversation_id: Optional[str] = Field(None, min_length=1, max_length=100)
    system_prompt: Optional[str] = Field(None, max_length=1000)
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


def require_token(authorization: Optional[str] = Header(None)) -> None:
    """Require a bearer token only when TOY_API_TOKEN is configured."""
    if not TOY_API_TOKEN:
        return
    expected = "Bearer " + TOY_API_TOKEN
    if authorization is None or not secrets.compare_digest(authorization, expected):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid API token")


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
    try:
        await run_in_threadpool(database.ensure_schema)
    except DatabaseUnavailable as error:
        raise RuntimeError("MySQL initialization failed") from error


async def ask_model(messages: List[Dict[str, str]], temperature: float) -> str:
    if not DEEPSEEK_API_KEY:
        raise HTTPException(status_code=503, detail="DeepSeek is not configured")

    payload = {"model": DEEPSEEK_MODEL, "messages": messages, "temperature": temperature}
    headers = {"Authorization": "Bearer " + DEEPSEEK_API_KEY, "Content-Type": "application/json"}
    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            response = await client.post(DEEPSEEK_BASE_URL + "/chat/completions", json=payload, headers=headers)
    except httpx.RequestError:
        raise HTTPException(
            status_code=503,
            detail="Cannot connect to DeepSeek. Check outbound HTTPS (TCP 443) or HTTPS_PROXY.",
        )

    if response.status_code >= 400:
        # Do not pass provider error text through: it may contain operational details.
        raise HTTPException(status_code=502, detail="AI provider returned an error")

    try:
        reply = response.json()["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, TypeError, ValueError, AttributeError):
        raise HTTPException(status_code=502, detail="AI provider returned an invalid response")
    if not reply:
        raise HTTPException(status_code=502, detail="AI provider returned an empty response")
    return reply


@app.get("/v1/health")
async def health():
    return {
        "status": "ok",
        "provider": "deepseek",
        "provider_configured": bool(DEEPSEEK_API_KEY),
        "memory_store": "mysql",
        "mysql_configured": database.configured,
    }


@app.post("/v1/chat/completions", response_model=ChatResponse, dependencies=[Depends(require_token)])
async def chat(request: ChatRequest):
    key = conversation_key(request)
    system_prompt = request.system_prompt or DEFAULT_SYSTEM_PROMPT

    history = await database_operation(
        database.get_history, request.device_id, key, MAX_HISTORY_MESSAGES
    )
    messages = [{"role": "system", "content": system_prompt}] + history + [
        {"role": "user", "content": request.message}
    ]
    reply = await ask_model(messages, request.temperature)
    await database_operation(database.save_turn, request.device_id, key, request.message, reply)

    return ChatResponse(conversation_id=key, reply=reply, model=DEEPSEEK_MODEL)


@app.delete("/v1/conversations/{conversation_id}", status_code=status.HTTP_204_NO_CONTENT,
            dependencies=[Depends(require_token)])
async def clear_conversation(conversation_id: str, device_id: str = Query(..., min_length=1, max_length=100)):
    await database_operation(database.clear_conversation, device_id.strip(), conversation_id)
