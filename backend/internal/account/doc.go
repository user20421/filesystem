// Package account 负责账号生命周期管理。
// 注销时先清理外部文件与对象，再删除用户及关联数据，失败时保留可重试状态。
package account
