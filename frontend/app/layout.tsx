import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "呆呆控制台 · AI 实体机器人测试",
  description: "用于调试 AI 实体机器人对话、记忆和情绪输出的本地控制台。",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="zh-CN">
      <body>{children}</body>
    </html>
  );
}
