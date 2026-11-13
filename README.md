# Secure IM：本地安全即时通讯与加密文件传输

一个可在 Windows 本地运行的企业级核心能力仿真项目：Qt 多用户客户端、Go 模块化单体后端，以及 Docker 部署的 MySQL、Redis、MinIO。

## 已实现

- 用户注册、登录、Argon2id 密码哈希、JWT 和 Refresh Token 轮换。
- 每用户支持“退出”（保留账号和数据）与“注销”（永久删除账号及关联消息、文件和凭据），均有确认提示。
- WebSocket + Protobuf 实时单聊、ACK、消息幂等、离线历史补拉。
- 联系人与会话显示对方在线状态；可见会话支持批量已读回执、离线已读持久化和多设备同步。
- 用户名统一大写；本地演示注册密码允许 6～128 字节（生产环境应提高密码强度）。
- 文件二进制分块上传、SHA-256 校验、上传续传、3～5 个文件并发限制。
- 每文件随机 DEK、AES-256-GCM 分块加密、KEK 密钥封装，MinIO 仅保存密文。
- 严格的发送者/接收者权限、取消删除、审计日志和真实集成测试。
- Qt 多用户工作台通过浏览器式标签随时切换用户；每个标签拥有独立会话、实时连接、消息历史和数据目录。
- 现代浅色主题、内嵌登录卡片、联系人搜索、聊天/传输独立视图、联系人未读红点及标签未读数。
- 传输进度、暂停/继续、取消/重试及接收文件自动下载。
- 单击文件名、本地路径或“打开目录”按钮即可打开文件所在文件夹。
- 下载通过顺序工作线程流式写入临时文件，校验大小后原子提交，避免整文件占用内存。

## 架构

```text
Qt 多用户工作台（系统测试 / 欢迎 / 独立用户标签）
                     │ REST/JSON + WebSocket/Protobuf + 二进制分块
                     ▼
               Go 模块化后端
                 ├─ MySQL：用户、消息、文件元数据、审计
                 ├─ Redis：本地基础设施（当前在线 Hub 为单实例内存实现）
                 └─ MinIO：AES-GCM 密文分块
```

文件内容不经过 JSON/Base64。当前是“传输与存储加密”，不是端到端加密；后端会在受控分块处理期间接触明文。

## 环境要求

- Windows PowerShell、Docker Desktop、Go 1.27+。
- Qt 6.11.2：`D:\software\Qt\6.11.2\llvm-mingw_64`。
- Qt 工具链已在本机检测到，无需全局安装 protoc；客户端脚本将其下载到 `client/.tools/`。

## 快速启动

以下命令均在项目根目录执行。

### 1. 启动基础设施

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\dev-up.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\doctor.ps1
```

为避免与本机其他项目冲突，使用端口：MySQL `13306`、Redis `16379`、MinIO `19000/19001`。首次启动会生成随机本地密码并写入被 Git 忽略的 `deploy/.env`。

### 2. 启动 Go 后端

打开新的 PowerShell 窗口：

```powershell
powershell -ExecutionPolicy Bypass -File .\backend\start-local.ps1
```

后端地址为 `http://127.0.0.1:8080`，启动时自动迁移数据库并检查 MinIO。密钥首次生成后会保存在 `deploy/.env`，重启不会使旧文件无法解密。

### 3. 构建并启动 Qt 多用户工作台

再打开一个 PowerShell 窗口：

```powershell
powershell -ExecutionPolicy Bypass -File .\client\start-simulator.ps1
```

顶部固定包含“系统测试”和“欢迎”标签，“系统测试”位于最左侧，后续用于大量用户消息与文件压力测试，当前暂不启用。

在欢迎页点击“新增用户”，或点击右上角 `+` 逐个创建。数据目录和服务器地址均为只读固定配置。每个标签都可独立注册和登录，例如 `USER_1`、`USER_2`，本地演示密码至少 6 字节，可使用 `123456`。用户名会统一显示为大写。

通过顶部标签随时切换用户，后台连接和文件传输不会中断。支持 `Ctrl+T` 新建、`Ctrl+W` 关闭、`Ctrl+Tab` 切换，也可以拖动标签排序。登录后选择对方即可聊天和发送文件。

用户数据固定保存在以下目录，欢迎页路径只读，不能误改：

```text
runtime/users/user_N/{inbox,outbox,downloads,temp,keys,logs,config}
```

MinIO 控制台为 `http://127.0.0.1:19001`，账号密码见 `deploy/.env`。不要提交该文件。

## 停止

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1
```

默认保留数据卷。`dev-down.ps1 -RemoveVolumes` 会永久删除本地 MySQL、Redis 和 MinIO 数据，请谨慎使用。

## 测试

```powershell
# Go 单元测试和静态检查
Set-Location backend
go test ./...
go vet ./...

# Qt 核心测试
Set-Location ..\client
& "D:/software/Qt/Tools/CMake_64/bin/ctest.exe" --preset qt-windows
```

后端和 Qt 均另有可选的真实服务集成测试。详细文档：

- [后端配置、API 与测试](backend/README.md)
- [Qt 客户端、标签生命周期与构建](client/README.md)

## 目录

```text
backend/   Go 服务、迁移、单元及真实 API 集成测试
client/    Qt Widgets 多用户工作台、单用户客户端、QtTest
proto/     共享 Protobuf Envelope
deploy/    Compose 与环境变量示例
scripts/   基础设施启动、停止、健康检查
runtime/   本地用户数据（不入库）
```

## 当前边界

- 默认仅监听回环地址，本地开发暂用 HTTP/WS；对外部署必须启用 HTTPS/WSS。
- 在线连接是单实例内存 Hub，Redis 尚未参与多实例消息分发。
- 上传支持分块续传；下载暂停后目前重新下载，尚未实现 Range。
- 未包含群聊、完整 Signal/E2EE、多节点容灾和生产级 KMS。
