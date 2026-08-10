import asyncio
import unittest
from unittest.mock import patch

from app.main import ask_model


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


if __name__ == "__main__":
    unittest.main()
