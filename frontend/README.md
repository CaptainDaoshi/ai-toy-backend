# 呆呆控制台

AI 实体机器人后端的本地测试前端，可测试：

- 后端健康状态与 MySQL / DeepSeek 配置
- 每设备 HMAC 令牌认证
- 首次聊天锁定正常或纯情绪输出模式
- 多会话、历史记录分页、临时 System Prompt 与 temperature
- 清空会话记忆与查看原始 JSON 响应

## 启动

需要 Node.js 22.13 或更高版本。先启动项目根目录的 FastAPI 后端，然后运行：

```powershell
cd frontend
npm install
npm run dev
```

打开命令行显示的本地地址。默认连接 `http://127.0.0.1:8000`。

设备令牌可在项目根目录生成：

```powershell
python scripts/generate_device_token.py toy-001
```

令牌不会写入前端代码。设备令牌默认按 `device_id` 保存在当前浏览器，适合个人电脑上的本地测试，并可在配置栏随时清除；可选的全局 API Token 仍只保存在 `sessionStorage`。

## 检查

```powershell
npm test
```
