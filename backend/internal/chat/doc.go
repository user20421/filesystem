// Package chat 提供单聊消息持久化、实时推送、历史同步、未读与已读回执。
// WebSocket 使用一次性票据，消息只有保存成功后才确认，ACK 与对端已读是不同状态。
package chat
