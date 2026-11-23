// Package store 负责 MySQL 连接、嵌入式迁移和审计记录。
// 迁移按文件顺序执行并记录版本，历史加密数据在非空时禁止直接切换到共享明文模型。
package store
