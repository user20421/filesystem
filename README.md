# Secure IM：本地即时通讯与文件传输

一个可在 Windows 本地运行的即时通讯项目：Qt 正式客户端、Go 模块化单体后端，以及 Docker 部署的 MySQL 与 MinIO。另有独立的 ROOT 本地压测入口。

## 已实现

- 用户注册、登录、Argon2id 密码哈希、短期 JWT、Refresh Token 轮换，以及注册/登录限流。
- 客户端自动刷新访问令牌并轮换 Refresh Token；“退出”撤销当前 Refresh Token 并清除本地会话，“注销”永久删除账号及关联消息、文件和凭据，均有确认提示。
- WebSocket + Protobuf v2 实时单聊、ACK、消息幂等、离线历史补拉；MESSAGE、ACK、ERROR 均使用对应 Protobuf payload，不再使用旧 JSON payload。
- 联系人与会话显示对方在线状态；可见会话支持批量已读回执、离线已读持久化和多连接同步。
- 用户名统一大写；本地注册密码允许 6～128 个 UTF-8 字节（生产环境应提高密码强度）。
- 文件二进制分块上传、逐块及整文件 SHA-256 校验、上传续传；正常服务与压测均默认 **5 MiB** 分块。
- 全系统按 **文件大小 + SHA-256** 复用已完成内容并秒传；不同逻辑文件共享内容引用，最后一个引用删除后才清理对象。
- MinIO 保存**明文分块**，不使用应用层 DEK/KEK 或密文分块；严格的发送者/接收者权限、取消删除、审计日志和真实集成测试。
- 正式客户端一个进程对应一个用户，可多开并使用独立数据目录；启动后注册或登录，也支持恢复本地会话。
- 客户端固定 3 条上传分块通道和 3 条下载文件通道。上传按剩余量动态调度并优先让新文件开始；公共 `QThreadPool` 默认 4 线程，单个下载文件由顺序 writer 流式落盘。
- 后端上传、Complete 和下载共享统一分块队列：多文件竞争时按文件单块轮转，新文件无需等待整个大文件；只剩一个文件时可利用空闲通道。默认最多 **96** 个分块槽位，并由 **500 MiB** 文件内存预算共同限制实际并发。
- 传输进度、暂停/继续、取消/重试及接收文件下载；单击文件名、本地路径或“打开目录”即可打开所在文件夹。
- ROOT 压测工具支持自定义在线人数、80/18/2 自动负载分配、实时指标、独立后端和带明确结论的历史报告。

## 架构

```text
Qt 正式客户端（一进程一用户，可多开）     ROOT 本地压测工具
                  │                              │
                  └──── REST + WebSocket ────────┘
                               │
                     Go 模块化单体后端
                     ├─ MySQL：用户、消息、文件元数据、审计
                     └─ MinIO：明文文件分块
```

聊天外层帧及业务 payload 均为 Protobuf v2；历史接口仅对完整 Envelope 字节做 Base64 传输。文件内容使用二进制分块，不经过 JSON/Base64。当前不含端到端加密或应用层存储加密。

## 环境要求

- Windows PowerShell、Docker Desktop、Go 1.27+。
- Qt 6.11.2：`D:\software\Qt\6.11.2\llvm-mingw_64`。
- 无需全局安装 protoc；客户端脚本将其下载到 `client/.tools/`。

## 快速启动

以下命令均在项目根目录执行。

### 1. 启动基础设施

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\dev-up.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\doctor.ps1
```

本地端口为 MySQL `13306`、MinIO `19000/19001`。首次启动会生成随机本地密码并写入被 Git 忽略的 `deploy/.env`。

### 2. 启动 Go 后端

```powershell
powershell -ExecutionPolicy Bypass -File .\backend\start-local.ps1
```

后端地址为 `http://127.0.0.1:8080`，启动时自动迁移数据库并检查 MinIO。JWT 密钥首次生成后保存在 `deploy/.env`，供重启后继续使用。

### 3. 构建并启动正式 Qt 客户端

```powershell
.\client\tools\fetch_protoc.ps1
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows -S client
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
```

在两个 PowerShell 窗口分别启动两个用户；每个进程必须使用不同的 `--data-dir`：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe --data-dir runtime/users/alice --server-url http://127.0.0.1:8080
```

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe --data-dir runtime/users/bob --server-url http://127.0.0.1:8080
```

启动后在各自窗口注册或登录。目录锁会拒绝两个进程共用同一数据目录；消息、配置、令牌和传输文件均按目录隔离。MinIO 控制台为 `http://127.0.0.1:19001`，账号密码见 `deploy/.env`，不要提交该文件。

## 系统压力测试

先启动正常 MySQL/MinIO，再运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\client\start-simulator.ps1
```

该程序是独立压测工具，不是正式多用户客户端。使用 **`ROOT` / `123456` 登录仅限此本地压测入口**；`scripts/stress-up.ps1` 会在独立压测后端上注册并验证该账号。默认配置：

- 80% 挂机、18% 聊天、2% 文件用户；每秒 200 个连接，稳定阶段 **5 分钟**。
- 独立后端仅监听 `127.0.0.1:18080`，限制 **6 CPU / 3 GiB**，文件内存背压 500 MiB。
- 100 MiB 压测文件、**5 MiB** 分块、后端与 runner 默认 **96** 个文件处理通道；该默认值来自本机固定 500 MiB 预算下按 16 递增的分档实测，runner 仍可用命令行参数覆盖。
- 独立数据库 `secure_im_stress` 和 Bucket `secure-im-stress-files`，但复用正常环境的 MySQL/MinIO 容器，不启动第二套基础设施，也不污染正常数据库和 Bucket。
- 压测 Compose 关闭注册/登录限流以完成批量账号准备；正常后端仍使用各自限流配置。

报告按时间保存在 `runtime/stress-reports/YYYYMMDDTHHMMSSZ-xxxxxx/`，包含 HTML、JSON、CSV、配置和结论。界面可查看历史报告或打开所在目录；结论区分成功、波动和失败，并给出瓶颈建议。

## 停止

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stress-down.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1
```

默认保留数据卷。`dev-down.ps1 -RemoveVolumes` 会永久删除本地 MySQL 和 MinIO 数据，请谨慎使用。

## 测试

```powershell
Set-Location backend
go test ./...
go vet ./...

Set-Location ..\client
& "D:/software/Qt/Tools/CMake_64/bin/ctest.exe" --preset qt-windows
```

后端和 Qt 均另有可选的真实服务集成测试。详细文档：

- [后端配置、API 与测试](backend/README.md)
- [Qt 客户端、压测工具与构建](client/README.md)

## 当前边界

- 默认仅监听回环地址，本地开发暂用 HTTP/WS；对外部署必须启用 HTTPS/WSS。
- 在线连接是单实例内存 Hub，不包含多实例消息分发。
- 上传支持分块续传；下载暂停后重新下载，尚未实现 Range。
- 未包含群聊、完整 Signal/E2EE、多节点容灾和生产级 KMS。
