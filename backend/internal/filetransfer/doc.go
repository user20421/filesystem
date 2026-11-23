// Package filetransfer 提供文件分块上传、校验、下载、秒传与异常清理。
// 逻辑文件引用与共享内容分离，MinIO 保存明文分块；调度槽位和内存预算共同限制实际并发。
package filetransfer
