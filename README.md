# AI 玩具对话后端

面向实体 AI 玩具的最小后端：设备上传文字，服务端保存会话上下文并转发给 DeepSeek。模型密钥只存在于服务器，设备端不需要、也不应该保存它。

## 启动

需要 Python 3.7+：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Copy-Item .env.example .env
# 编辑 .env，填入你的 DEEPSEEK_API_KEY（或 DEEPSEEK_API_TOKEN）
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

打开 `http://127.0.0.1:8000/docs` 可试用交互式 API 文档。

若收到 `503 DeepSeek is not configured`，说明运行中的服务没有读取到密钥。确认项目根目录下存在名为 `.env` 的文件（不是 `.env.example` 或 `.env.txt`），其中有一行 `DEEPSEEK_API_KEY=你的真实密钥`，保存后重启 uvicorn。不要把密钥发给我，也不要提交 `.env` 到 Git。

默认使用 `deepseek-v4-flash`，适合对话玩具的快速回复；若更看重能力，可把 `.env` 的 `DEEPSEEK_MODEL` 改为 `deepseek-v4-pro`。DeepSeek 使用与 OpenAI Chat Completions 兼容的 `/chat/completions` 接口，基地址应为 `https://api.deepseek.com`（不要额外加 `/v1`）。[DeepSeek 官方快速开始](https://api-docs.deepseek.com/quick_start/pricing-details-usd/)

## 持久化聊天记忆（MySQL）

服务现在将每轮用户和助手消息写入 MySQL，重启后也能继续同一段对话。创建数据库和账号后，在 `.env` 填入 `MYSQL_HOST`、`MYSQL_PORT`、`MYSQL_USER`、`MYSQL_PASSWORD`、`MYSQL_DATABASE`；字段模板已在 `.env.example` 中提供。应用启动时会自动创建 `chat_messages` 和 `conversation_memories` 表。如果 MySQL 不可连接，服务会在启动时报错，而不是悄悄以无记忆模式运行。

### 分层记忆：避免长对话失焦

后端不再把不断增长的完整聊天记录塞进模型上下文，而是采用类似海马体的记忆巩固流程：

1. **工作记忆**：只保留最近 `WORKING_MEMORY_MESSAGES` 条原始消息，默认 8 条。
2. **情景巩固**：更早的消息分批交给模型提炼，写入 `conversation_memories` 的长期摘要。
3. **受限提取**：每次聊天发送“角色设定 + 长期摘要 + 近期消息 + 当前输入”，长期摘要默认最多 4,000 字，不会无限变长。

此外还有硬上下文预算：`MODEL_CONTEXT_MAX_TOKENS` 默认按 32K 配置，预留回复空间和安全余量后再组装输入。角色 Prompt 与当前用户消息拥有最高保留优先级；长期摘要会受限截断，近期消息则从最新到最旧按剩余预算加入。模型请求中还会在用户输入前放置一条很短的身份锚点，减轻长上下文中的“中间信息遗失”和角色漂移。最终请求不会随着数据库记录数量增长。

可在 `.env` 调整 `WORKING_MEMORY_MESSAGES`、`MEMORY_SUMMARY_MAX_CHARS` 等参数；完整示例见 `.env.example`。首次处理已有的大量历史时，每次请求最多巩固 3 批，避免额外请求拖慢玩具回应；后续对话会逐步完成压缩。

例如，以管理员账号创建本地开发数据库：

```sql
CREATE DATABASE ai_toy CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'ai_toy'@'localhost' IDENTIFIED BY '请使用一段强密码';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX ON ai_toy.* TO 'ai_toy'@'localhost';
FLUSH PRIVILEGES;
```

安装新增依赖并重启服务：

```powershell
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

同一 `device_id` 会默认延续记忆；若设备使用 `conversation_id`，记忆会按设备和会话 ID 隔离。删除记忆时必须带上设备 ID，例如：`DELETE /v1/conversations/toy-001?device_id=toy-001`。

## 无法连接 DeepSeek

如果返回 `Cannot connect to DeepSeek`，先在运行后端的电脑上确认允许出站访问 `api.deepseek.com` 的 TCP 443 端口。若网络必须使用 HTTP/HTTPS 代理，在 `.env` 中设置 `HTTPS_PROXY=http://代理地址:端口` 后重启服务；程序会自动使用该环境变量。密钥有效但网络不通时，仍会返回 503。

## 玩具调用

```http
POST /v1/chat/completions
Content-Type: application/json
Authorization: Bearer <TOY_API_TOKEN>  # 未设置令牌时可省略
X-Device-Token: <该 device_id 对应的设备令牌>

{
  "device_id": "toy-001",
  "message": "你好，你叫什么名字？",
  "response_mode": "normal"
}
```

响应中的 `reply` 可直接交给设备的 TTS（文字转语音）模块朗读。相同的 `device_id` 会自动延续对话；若要隔离多个孩子或角色，可额外提供 `conversation_id`。

### 设备输出模式

设备第一次成功聊天时必须通过 `response_mode` 选择输出方式：

- `normal`：正常聊天文本。
- `emotion`：只返回 `平静、开心、生气、烦恼、难过、害羞、好奇、惊讶、困倦、委屈` 中的一个标签，响应的 `emotion` 字段会给出同一个值。

选择只会在模型成功回复后，和首轮聊天记录一起按 `device_id` 原子写入 `device_profiles` 表。失败请求不会锁定模式。此后请求可以不再传 `response_mode`，服务会自动沿用；如果显式传入另一模式，服务返回 `409 Conflict`，不会切换。`conversation_id` 的变化不会改变设备模式。

### 每设备鉴权

服务启动前必须在 `.env` 设置至少 32 个字符的随机 `DEVICE_AUTH_SECRET`。设备令牌由服务端主密钥和 `device_id` 通过 HMAC-SHA256 派生，不需要在数据库保存明文设备密钥：

```powershell
python scripts/generate_device_token.py device-1
```

把输出令牌安全写入对应实体设备。聊天和删除记忆请求都必须通过 `X-Device-Token` 请求头携带它。令牌与 `device_id` 一一绑定，因此其他设备即使知道该 ID 也无法抢先锁定它的输出模式。`DEVICE_AUTH_SECRET` 只能保存在服务器，不得写入设备固件或提交到 Git。

## 端点

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/v1/health` | 健康检查，不泄露配置 |
| POST | `/v1/chat/completions` | 发送一句话并取得回复 |
| DELETE | `/v1/conversations/{conversation_id}?device_id=...` | 清除指定设备的一段会话记忆 |

`/v1/chat/completions` 可选字段：`conversation_id`、`system_prompt`（角色设定）、`temperature`（0–2）。服务端会限制单次输入为 2,000 个字符，保留最近 20 条历史消息。

## 上线前建议

- 用 HTTPS，并为每台设备或每批设备分配独立令牌。
- 定期删除过期聊天记录，并为数据库开启备份与访问控制。
- 产品面向全年龄用户；应根据实际用户增加内容安全、隐私同意、数据留存与删除机制。
