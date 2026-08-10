import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", { headers: { accept: "text/html" } }),
    { ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) } },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the AI toy console", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>呆呆控制台 · AI 实体机器人测试<\/title>/i);
  assert.match(html, /呆呆控制台/);
  assert.match(html, /收起配置/);
  assert.match(html, /aria-controls="connection-settings"/);
  assert.match(html, /正常聊天/);
  assert.match(html, /纯情绪/);
});

test("loads history, manages local device tokens, and calls the intended API", async () => {
  const [page, css] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/globals.css", import.meta.url), "utf8"),
  ]);

  assert.match(page, /"use client"/);
  assert.match(page, /sessionStorage\.setItem\("ai-toy-device-token"/);
  assert.match(page, /DEVICE_TOKEN_VAULT_KEY/);
  assert.match(page, /localStorage\.setItem\(DEVICE_TOKEN_VAULT_KEY/);
  assert.match(page, /forgetDeviceToken/);
  assert.match(page, /X-Device-Token/);
  assert.match(page, /\/v1\/devices\/\$\{encodeURIComponent\(deviceId\.trim\(\)\)\}\/profile/);
  assert.match(page, /\/v1\/chat\/completions/);
  assert.match(page, /\/messages\?\$\{query\.toString\(\)\}/);
  assert.match(page, /\/v1\/conversations\//);
  assert.match(page, /workspace-settings-closed/);
  assert.doesNotMatch(page, /DEEPSEEK_API_KEY/);
  assert.match(css, /prefers-reduced-motion:\s*reduce/);
});
