"use client";

import {
  type FormEvent,
  type KeyboardEvent,
  useEffect,
  useRef,
  useState,
} from "react";

type ResponseMode = "normal" | "emotion";
type ConnectionState = "idle" | "checking" | "online" | "error";

type DeviceProfile = {
  device_id: string;
  initialized: boolean;
  response_mode: ResponseMode | null;
};

type HealthResponse = {
  status: string;
  provider: string;
  provider_configured: boolean;
  memory_store: string;
  mysql_configured: boolean;
};

type ChatResponse = {
  conversation_id: string;
  reply: string;
  model: string;
  response_mode: ResponseMode;
  emotion: string | null;
};

type StoredMessage = {
  id: number;
  role: "user" | "assistant";
  content: string;
  created_at: string;
};

type HistoryResponse = {
  device_id: string;
  conversation_id: string;
  messages: StoredMessage[];
  has_more: boolean;
  next_before_id: number | null;
};

type ChatMessage = {
  id: string;
  role: "user" | "robot" | "notice";
  content: string;
  emotion?: string | null;
  createdAt?: string;
  failed?: boolean;
};

const DEVICE_TOKEN_VAULT_KEY = "ai-toy-device-token-vault";

const emotionAppearance: Record<string, { mark: string; tone: string }> = {
  平静: { mark: "—", tone: "calm" },
  开心: { mark: "◡", tone: "happy" },
  生气: { mark: "!", tone: "angry" },
  烦恼: { mark: "≈", tone: "worried" },
  难过: { mark: "⌒", tone: "sad" },
  害羞: { mark: "·", tone: "shy" },
  好奇: { mark: "?", tone: "curious" },
  惊讶: { mark: "○", tone: "surprised" },
  困倦: { mark: "z", tone: "sleepy" },
  委屈: { mark: "…", tone: "hurt" },
};

function trimBaseUrl(value: string) {
  return value.trim().replace(/\/+$/, "");
}

function messageId() {
  return `${Date.now()}-${Math.random().toString(36).slice(2)}`;
}

function readDeviceTokenVault(): Record<string, string> {
  try {
    const parsed = JSON.parse(localStorage.getItem(DEVICE_TOKEN_VAULT_KEY) || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function formatMessageTime(value?: string) {
  if (!value) return "";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "";
  return new Intl.DateTimeFormat("zh-CN", {
    month: "numeric",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

async function readError(response: Response) {
  try {
    const body = (await response.json()) as { detail?: string };
    return body.detail || `请求失败（${response.status}）`;
  } catch {
    return `请求失败（${response.status}）`;
  }
}

export default function Home() {
  const [apiUrl, setApiUrl] = useState("http://127.0.0.1:8000");
  const [deviceId, setDeviceId] = useState("toy-001");
  const [deviceToken, setDeviceToken] = useState("");
  const [apiToken, setApiToken] = useState("");
  const [conversationId, setConversationId] = useState("");
  const [selectedMode, setSelectedMode] = useState<ResponseMode>("normal");
  const [temperature, setTemperature] = useState(0.7);
  const [systemPrompt, setSystemPrompt] = useState("");
  const [profile, setProfile] = useState<DeviceProfile | null>(null);
  const [health, setHealth] = useState<HealthResponse | null>(null);
  const [connectionState, setConnectionState] =
    useState<ConnectionState>("idle");
  const [connectionText, setConnectionText] = useState("等待检查连接");
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [composer, setComposer] = useState("");
  const [sending, setSending] = useState(false);
  const [rawResponse, setRawResponse] = useState<unknown>(null);
  const [rawOpen, setRawOpen] = useState(false);
  const [settingsOpen, setSettingsOpen] = useState(true);
  const [rememberDeviceToken, setRememberDeviceToken] = useState(true);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyHasMore, setHistoryHasMore] = useState(false);
  const [historyBeforeId, setHistoryBeforeId] = useState<number | null>(null);
  const [hydrated, setHydrated] = useState(false);
  const messagesEndRef = useRef<HTMLDivElement>(null);
  const shouldScrollToBottomRef = useRef(true);

  useEffect(() => {
    let cancelled = false;
    queueMicrotask(() => {
      if (cancelled) return;
      const storedDeviceId = localStorage.getItem("ai-toy-device-id") || "toy-001";
      const vault = readDeviceTokenVault();
      const legacySessionToken = sessionStorage.getItem("ai-toy-device-token") || "";
      const storedDeviceToken = vault[storedDeviceId] || legacySessionToken;
      if (legacySessionToken && !vault[storedDeviceId]) {
        vault[storedDeviceId] = legacySessionToken;
        localStorage.setItem(DEVICE_TOKEN_VAULT_KEY, JSON.stringify(vault));
      }
      setApiUrl(localStorage.getItem("ai-toy-api-url") || "http://127.0.0.1:8000");
      setDeviceId(storedDeviceId);
      setConversationId(localStorage.getItem("ai-toy-conversation-id") || "");
      setSelectedMode(
        (localStorage.getItem("ai-toy-response-mode") as ResponseMode) || "normal",
      );
      setDeviceToken(storedDeviceToken);
      setApiToken(sessionStorage.getItem("ai-toy-api-token") || "");
      setHydrated(true);
    });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (!hydrated) return;
    localStorage.setItem("ai-toy-api-url", apiUrl);
    localStorage.setItem("ai-toy-device-id", deviceId);
    localStorage.setItem("ai-toy-conversation-id", conversationId);
    localStorage.setItem("ai-toy-response-mode", selectedMode);
    sessionStorage.setItem("ai-toy-device-token", deviceToken);
    sessionStorage.setItem("ai-toy-api-token", apiToken);
    if (deviceId.trim()) {
      const vault = readDeviceTokenVault();
      if (rememberDeviceToken && deviceToken.trim()) {
        vault[deviceId.trim()] = deviceToken.trim();
      } else {
        delete vault[deviceId.trim()];
      }
      localStorage.setItem(DEVICE_TOKEN_VAULT_KEY, JSON.stringify(vault));
    }
  }, [
    apiToken,
    apiUrl,
    conversationId,
    deviceId,
    deviceToken,
    hydrated,
    rememberDeviceToken,
    selectedMode,
  ]);

  useEffect(() => {
    if (shouldScrollToBottomRef.current) {
      messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
    }
    shouldScrollToBottomRef.current = true;
  }, [messages, sending]);

  function markConnectionDirty() {
    setProfile(null);
    setConnectionState("idle");
    setConnectionText("配置已变化，请重新检查");
  }

  function changeDeviceId(nextDeviceId: string) {
    const normalizedDeviceId = nextDeviceId.trim();
    setDeviceId(nextDeviceId);
    setDeviceToken(normalizedDeviceId ? readDeviceTokenVault()[normalizedDeviceId] || "" : "");
    setMessages([]);
    setHistoryHasMore(false);
    setHistoryBeforeId(null);
    markConnectionDirty();
  }

  function changeConversationId(nextConversationId: string) {
    setConversationId(nextConversationId);
    setMessages([]);
    setHistoryHasMore(false);
    setHistoryBeforeId(null);
    setConnectionState("idle");
    setConnectionText("会话已变化，请重新检查以加载历史记录");
  }

  function forgetDeviceToken() {
    const vault = readDeviceTokenVault();
    delete vault[deviceId.trim()];
    localStorage.setItem(DEVICE_TOKEN_VAULT_KEY, JSON.stringify(vault));
    sessionStorage.removeItem("ai-toy-device-token");
    setDeviceToken("");
    setRememberDeviceToken(false);
    markConnectionDirty();
  }

  function authHeaders(json = false): HeadersInit {
    const headers: Record<string, string> = {};
    if (json) headers["Content-Type"] = "application/json";
    if (deviceToken.trim()) headers["X-Device-Token"] = deviceToken.trim();
    if (apiToken.trim()) headers.Authorization = `Bearer ${apiToken.trim()}`;
    return headers;
  }

  async function fetchProfile(): Promise<DeviceProfile> {
    if (!deviceId.trim()) throw new Error("请填写 device_id");
    if (!deviceToken.trim()) throw new Error("请填写设备令牌");
    const response = await fetch(
      `${trimBaseUrl(apiUrl)}/v1/devices/${encodeURIComponent(deviceId.trim())}/profile`,
      { headers: authHeaders() },
    );
    if (!response.ok) throw new Error(await readError(response));
    const nextProfile = (await response.json()) as DeviceProfile;
    setProfile(nextProfile);
    if (nextProfile.response_mode) setSelectedMode(nextProfile.response_mode);
    return nextProfile;
  }

  async function loadHistory(reset: boolean, mode: ResponseMode): Promise<number> {
    const activeConversation = conversationId.trim() || deviceId.trim();
    if (!activeConversation || !deviceId.trim()) return 0;
    if (!reset && historyBeforeId === null) return 0;

    setHistoryLoading(true);
    try {
      const query = new URLSearchParams({
        device_id: deviceId.trim(),
        limit: "50",
      });
      if (!reset && historyBeforeId !== null) {
        query.set("before_id", String(historyBeforeId));
      }
      const response = await fetch(
        `${trimBaseUrl(apiUrl)}/v1/conversations/${encodeURIComponent(activeConversation)}/messages?${query.toString()}`,
        { headers: authHeaders() },
      );
      if (!response.ok) throw new Error(await readError(response));
      const result = (await response.json()) as HistoryResponse;
      const storedMessages: ChatMessage[] = result.messages.map((message) => ({
        id: `stored-${message.id}`,
        role: message.role === "assistant" ? "robot" : "user",
        content: message.content,
        emotion:
          mode === "emotion" && emotionAppearance[message.content]
            ? message.content
            : null,
        createdAt: message.created_at,
      }));

      shouldScrollToBottomRef.current = reset;
      if (reset) {
        setMessages(storedMessages);
      } else {
        setMessages((current) => {
          const existingIds = new Set(current.map((message) => message.id));
          return [
            ...storedMessages.filter((message) => !existingIds.has(message.id)),
            ...current,
          ];
        });
      }
      setHistoryHasMore(result.has_more);
      setHistoryBeforeId(result.next_before_id);
      return storedMessages.length;
    } finally {
      setHistoryLoading(false);
    }
  }

  async function checkConnection() {
    setConnectionState("checking");
    setConnectionText("正在敲后端的门…");
    try {
      const response = await fetch(`${trimBaseUrl(apiUrl)}/v1/health`);
      if (!response.ok) throw new Error(await readError(response));
      const nextHealth = (await response.json()) as HealthResponse;
      setHealth(nextHealth);

      if (deviceId.trim() && deviceToken.trim()) {
        const nextProfile = await fetchProfile();
        const historyCount = await loadHistory(
          true,
          nextProfile.response_mode || selectedMode,
        );
        setConnectionText(
          nextProfile.initialized
            ? `设备已连接，${nextProfile.response_mode === "emotion" ? "纯情绪" : "正常聊天"}模式已锁定 · 已加载 ${historyCount} 条记录`
            : "设备已连接，暂无历史记录；首次聊天将锁定所选模式",
        );
      } else {
        setProfile(null);
        setConnectionText("后端在线；填写设备令牌后可开始聊天");
      }
      setConnectionState("online");
    } catch (error) {
      setConnectionState("error");
      setConnectionText(error instanceof Error ? error.message : "连接失败");
    }
  }

  async function sendMessage(event?: FormEvent) {
    event?.preventDefault();
    const text = composer.trim();
    if (!text || sending) return;
    if (!trimBaseUrl(apiUrl) || !deviceId.trim() || !deviceToken.trim()) {
      setConnectionState("error");
      setConnectionText("API 地址、device_id 和设备令牌都需要填写");
      setSettingsOpen(true);
      return;
    }

    const userMessage: ChatMessage = {
      id: messageId(),
      role: "user",
      content: text,
    };
    shouldScrollToBottomRef.current = true;
    setMessages((current) => [...current, userMessage]);
    setComposer("");
    setSending(true);
    setRawResponse(null);

    try {
      const currentProfile = profile || (await fetchProfile());
      const payload: Record<string, unknown> = {
        device_id: deviceId.trim(),
        message: text,
        temperature,
      };
      if (conversationId.trim()) payload.conversation_id = conversationId.trim();
      if (systemPrompt.trim()) payload.system_prompt = systemPrompt.trim();
      if (!currentProfile.initialized) payload.response_mode = selectedMode;

      const response = await fetch(`${trimBaseUrl(apiUrl)}/v1/chat/completions`, {
        method: "POST",
        headers: authHeaders(true),
        body: JSON.stringify(payload),
      });
      if (!response.ok) throw new Error(await readError(response));

      const result = (await response.json()) as ChatResponse;
      setRawResponse(result);
      setConversationId(result.conversation_id);
      setSelectedMode(result.response_mode);
      setProfile({
        device_id: deviceId.trim(),
        initialized: true,
        response_mode: result.response_mode,
      });
      setConnectionState("online");
      setConnectionText(
        result.response_mode === "emotion"
          ? "设备在线 · 纯情绪模式"
          : "设备在线 · 正常聊天模式",
      );
      setMessages((current) => [
        ...current,
        {
          id: messageId(),
          role: "robot",
          content: result.reply,
          emotion: result.emotion,
        },
      ]);
    } catch (error) {
      const errorText = error instanceof Error ? error.message : "请求失败";
      setRawResponse({ error: errorText });
      setConnectionState("error");
      setConnectionText(errorText);
      setMessages((current) => [
        ...current,
        {
          id: messageId(),
          role: "notice",
          content: `这次没有听清：${errorText}`,
          failed: true,
        },
      ]);
    } finally {
      setSending(false);
    }
  }

  async function clearConversation() {
    const activeConversation = conversationId.trim() || deviceId.trim();
    if (!activeConversation || !deviceId.trim() || !deviceToken.trim()) {
      setConnectionState("error");
      setConnectionText("缺少会话或设备认证信息");
      return;
    }
    if (!window.confirm(`清空会话“${activeConversation}”的聊天记录和记忆？`)) return;

    try {
      const response = await fetch(
        `${trimBaseUrl(apiUrl)}/v1/conversations/${encodeURIComponent(activeConversation)}?device_id=${encodeURIComponent(deviceId.trim())}`,
        { method: "DELETE", headers: authHeaders() },
      );
      if (!response.ok) throw new Error(await readError(response));
      setMessages([]);
      setHistoryHasMore(false);
      setHistoryBeforeId(null);
      setRawResponse(null);
      setConnectionState("online");
      setConnectionText("当前会话已清空；设备模式仍保持锁定");
    } catch (error) {
      setConnectionState("error");
      setConnectionText(error instanceof Error ? error.message : "清空失败");
    }
  }

  async function loadOlderHistory() {
    try {
      await loadHistory(false, profile?.response_mode || selectedMode);
    } catch (error) {
      setConnectionState("error");
      setConnectionText(error instanceof Error ? error.message : "历史记录加载失败");
    }
  }

  function handleComposerKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      void sendMessage();
    }
  }

  const activeMode = profile?.response_mode || selectedMode;
  const activeEmotion =
    messages.slice().reverse().find((message) => message.emotion)?.emotion || "平静";
  const emotionStyle = emotionAppearance[activeEmotion] || emotionAppearance.平静;

  return (
    <main className="app-shell">
      <header className="topbar">
        <a className="brand" href="#top" aria-label="呆呆控制台首页">
          <span className="brand-mark" aria-hidden="true">D</span>
          <span>
            <strong>呆呆控制台</strong>
            <small>LOCAL TEST BENCH</small>
          </span>
        </a>
        <div className={`connection connection-${connectionState}`} role="status">
          <span className="status-light" />
          <span>{connectionText}</span>
        </div>
        <button
          className="settings-toggle"
          type="button"
          aria-expanded={settingsOpen}
          aria-controls="connection-settings"
          onClick={() => setSettingsOpen(!settingsOpen)}
        >
          {settingsOpen ? "收起配置" : "连接配置"}
        </button>
      </header>

      <div className={`workspace ${settingsOpen ? "workspace-settings-open" : "workspace-settings-closed"}`} id="top">
        <aside id="connection-settings" className={`settings-panel ${settingsOpen ? "settings-open" : ""}`}>
          <div className="panel-heading">
            <span className="eyebrow">01 / CONNECTION</span>
            <h2>把它接上电</h2>
            <p>连接后自动读取历史记录；设备令牌可以按设备保存在这台电脑上。</p>
          </div>

          <div className="field-stack">
            <label>
              <span>后端地址</span>
              <input value={apiUrl} onChange={(event) => { setApiUrl(event.target.value); markConnectionDirty(); }} placeholder="http://127.0.0.1:8000" />
            </label>
            <label>
              <span>device_id</span>
              <input value={deviceId} onChange={(event) => changeDeviceId(event.target.value)} placeholder="toy-001" />
            </label>
            <div className="field-group">
              <span>X-Device-Token</span>
              <input aria-label="X-Device-Token" type="password" value={deviceToken} onChange={(event) => { setDeviceToken(event.target.value); markConnectionDirty(); }} placeholder="粘贴设备令牌" autoComplete="off" />
              <div className="token-tools">
                <label className="remember-token">
                  <input
                    type="checkbox"
                    checked={rememberDeviceToken}
                    onChange={(event) => setRememberDeviceToken(event.target.checked)}
                  />
                  <span>在这台电脑上记住</span>
                </label>
                {deviceToken && <button type="button" onClick={forgetDeviceToken}>忘记令牌</button>}
              </div>
              <small className="security-note">仅建议在你的个人电脑上开启，不要在共享设备上保存。</small>
            </div>
            <label>
              <span>全局 API Token <em>可选</em></span>
              <input type="password" value={apiToken} onChange={(event) => { setApiToken(event.target.value); markConnectionDirty(); }} placeholder="TOY_API_TOKEN 未设置时留空" autoComplete="off" />
            </label>
            <label>
              <span>conversation_id <em>可选</em></span>
              <input value={conversationId} onChange={(event) => changeConversationId(event.target.value)} placeholder="默认使用 device_id" />
            </label>
          </div>

          <section className="mode-section" aria-labelledby="mode-title">
            <div className="field-title" id="mode-title">
              首次回复模式
              {profile?.initialized && <span className="locked-label">已锁定</span>}
            </div>
            <div className="mode-switch">
              <button
                type="button"
                className={activeMode === "normal" ? "active" : ""}
                onClick={() => setSelectedMode("normal")}
                disabled={Boolean(profile?.initialized)}
              >
                <strong>正常聊天</strong>
                <small>完整语言回复</small>
              </button>
              <button
                type="button"
                className={activeMode === "emotion" ? "active" : ""}
                onClick={() => setSelectedMode("emotion")}
                disabled={Boolean(profile?.initialized)}
              >
                <strong>纯情绪</strong>
                <small>只返回情绪标签</small>
              </button>
            </div>
            <p className="mode-note">
              {profile?.initialized
                ? "这台设备已完成首次对话，模式不可更改。"
                : "首次成功聊天后，这台设备会永久绑定所选模式。"}
            </p>
          </section>

          <details className="advanced-settings">
            <summary>高级参数</summary>
            <label>
              <span>Temperature · {temperature.toFixed(1)}</span>
              <input type="range" min="0" max="2" step="0.1" value={temperature} onChange={(event) => setTemperature(Number(event.target.value))} disabled={activeMode === "emotion"} />
            </label>
            <label>
              <span>临时 System Prompt <em>可选</em></span>
              <textarea value={systemPrompt} onChange={(event) => setSystemPrompt(event.target.value)} maxLength={1000} placeholder="留空时使用后端默认机器人设定" rows={4} />
            </label>
          </details>

          <button className="primary-button full-width" type="button" onClick={checkConnection} disabled={connectionState === "checking"}>
            {connectionState === "checking" ? "正在连接…" : "检查连接"}
          </button>

          {health && (
            <dl className="health-grid">
              <div><dt>MODEL</dt><dd>{health.provider}</dd></div>
              <div><dt>MEMORY</dt><dd>{health.memory_store}</dd></div>
              <div><dt>API</dt><dd>{health.provider_configured ? "READY" : "MISSING"}</dd></div>
              <div><dt>MYSQL</dt><dd>{health.mysql_configured ? "READY" : "MISSING"}</dd></div>
            </dl>
          )}
        </aside>

        <section className="chat-stage" aria-label="机器人聊天测试">
          <div className="robot-strip">
            <div className={`robot-avatar robot-${emotionStyle.tone}`} aria-label={`机器人当前情绪：${activeEmotion}`}>
              <span className="antenna" />
              <span className="robot-eye eye-left" />
              <span className="robot-eye eye-right" />
              <span className="robot-mouth">{emotionStyle.mark}</span>
            </div>
            <div className="robot-intro">
              <span className="eyebrow">02 / LIVE SESSION</span>
              <h1>{activeMode === "emotion" ? "它现在只说感受。" : "它醒着，有点慢半拍。"}</h1>
              <p>
                {activeMode === "emotion"
                  ? "每次回复只包含一个受控情绪标签，适合测试灯光、动作和表情联动。"
                  : "连接设备后会读取已保存的聊天记录；后端仍会自动携带近期对话与长期记忆。"}
              </p>
            </div>
            <div className="session-meta">
              <span>DEVICE</span>
              <strong>{deviceId || "未设置"}</strong>
              <span>CONVERSATION</span>
              <strong title={conversationId || deviceId}>{conversationId || deviceId || "未设置"}</strong>
            </div>
          </div>

          <div className="message-window">
            {messages.length === 0 ? (
              <div className="empty-state">
                <span className="empty-line" />
                <p>还没有声音。</p>
                <small>先检查连接，然后和它说点什么。</small>
                <div className="prompt-suggestions">
                  {["你好，你叫什么名字？", "你今天自己做了什么？", "还记得我之前说过什么吗？"].map((suggestion) => (
                    <button key={suggestion} type="button" onClick={() => setComposer(suggestion)}>{suggestion}</button>
                  ))}
                </div>
              </div>
            ) : (
              <div className="message-list">
                {historyHasMore && (
                  <button className="history-more" type="button" onClick={loadOlderHistory} disabled={historyLoading}>
                    {historyLoading ? "正在读取…" : "加载更早的 50 条记录"}
                  </button>
                )}
                {messages.map((message) => {
                  const appearance = message.emotion ? emotionAppearance[message.emotion] : null;
                  return (
                    <article key={message.id} className={`message-row message-${message.role} ${message.failed ? "message-failed" : ""}`}>
                      <span className="message-label">
                        {message.role === "user" ? "YOU" : message.role === "robot" ? "DAIDAI" : "SYSTEM"}
                      </span>
                      <div className="message-bubble">
                        {appearance && <span className={`emotion-mark emotion-${appearance.tone}`}>{appearance.mark}</span>}
                        <p>{message.content}</p>
                        {message.createdAt && <time dateTime={message.createdAt}>{formatMessageTime(message.createdAt)}</time>}
                      </div>
                    </article>
                  );
                })}
                {sending && (
                  <article className="message-row message-robot">
                    <span className="message-label">DAIDAI</span>
                    <div className="message-bubble thinking"><span /><span /><span /></div>
                  </article>
                )}
              </div>
            )}
            <div ref={messagesEndRef} />
          </div>

          <form className="composer" onSubmit={sendMessage}>
            <textarea
              value={composer}
              onChange={(event) => setComposer(event.target.value)}
              onKeyDown={handleComposerKeyDown}
              placeholder="对它说点什么…"
              maxLength={2000}
              rows={1}
              aria-label="聊天消息"
            />
            <div className="composer-actions">
              <span>{composer.length} / 2000 · Enter 发送 · Shift+Enter 换行</span>
              <button type="submit" disabled={!composer.trim() || sending} aria-label="发送消息">
                {sending ? "等待" : "发送 ↗"}
              </button>
            </div>
          </form>

          <footer className="debug-bar">
            <button type="button" onClick={() => setRawOpen(!rawOpen)}>
              {rawOpen ? "收起原始响应" : "查看原始响应"}
            </button>
            <span />
            <button className="danger-link" type="button" onClick={clearConversation}>清空当前会话</button>
          </footer>

          {rawOpen && (
            <section className="raw-panel" aria-label="原始 API 响应">
              <div><span>RAW RESPONSE</span><button type="button" onClick={() => setRawResponse(null)}>清除</button></div>
              <pre>{rawResponse ? JSON.stringify(rawResponse, null, 2) : "// 发送消息后，这里会显示完整 JSON"}</pre>
            </section>
          )}
        </section>
      </div>
    </main>
  );
}
