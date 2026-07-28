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

## 无法连接 DeepSeek

如果返回 `Cannot connect to DeepSeek`，先在运行后端的电脑上确认允许出站访问 `api.deepseek.com` 的 TCP 443 端口。若网络必须使用 HTTP/HTTPS 代理，在 `.env` 中设置 `HTTPS_PROXY=http://代理地址:端口` 后重启服务；程序会自动使用该环境变量。密钥有效但网络不通时，仍会返回 503。

## 玩具调用

```http
POST /v1/chat/completions
Content-Type: application/json
Authorization: Bearer <TOY_API_TOKEN>  # 未设置令牌时可省略

{
  "device_id": "toy-001",
  "message": "你好，你叫什么名字？"
}
```

响应中的 `reply` 可直接交给设备的 TTS（文字转语音）模块朗读。相同的 `device_id` 会自动延续对话；若要隔离多个孩子或角色，可额外提供 `conversation_id`。

## 端点

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/v1/health` | 健康检查，不泄露配置 |
| POST | `/v1/chat/completions` | 发送一句话并取得回复 |
| DELETE | `/v1/conversations/{conversation_id}` | 清除一段会话记忆 |

`/v1/chat/completions` 可选字段：`conversation_id`、`system_prompt`（角色设定）、`temperature`（0–2）。服务端会限制单次输入为 2,000 个字符，保留最近 20 条历史消息。

## 上线前建议

- 用 HTTPS，并为每台设备或每批设备分配独立令牌。
- 生产环境把内存会话替换为 Redis 或数据库；当前版本重启后会清空记忆。
- 若玩具面向儿童，请增加内容安全策略、家长同意、数据留存与删除机制。
