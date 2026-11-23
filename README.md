# Secure IM：本地即时通讯与文件传输

Secure IM 是一个面向 Windows 本地开发与真实负载验证的即时通讯项目，包含 Qt 6 Widgets 客户端、Go 模块化单体后端，以及由 Docker Compose 管理的 MySQL 和 MinIO。正式客户端支持单聊、文件传输和集成 ROOT 系统压测；另保留独立 simulator 兼容入口。

## 定位与安全边界

- 默认服务仅绑定回环地址，本地开发使用明文 **HTTP/WS**，不是可直接暴露到公网的生产部署；对外部署必须启用 HTTPS/WSS 并完成相应安全加固。
- MinIO 保存的是**明文文件分块**，服务端能够读取文件内容；当前没有端到端加密（E2EE），访问权限控制不等同于内容保密。
- 在线状态和实时消息投递依赖**单实例内存 Hub**，没有跨实例消息分发或多节点容灾。
- MySQL 持久化用户、消息、文件元数据和审计信息；文件内容通过二进制分块传输，不经过 JSON/Base64。

## 功能矩阵

| 模块 | 当前能力 |
|---|---|
| 认证与会话 | 注册、登录、Argon2id 密码哈希、短期 JWT、Refresh Token 轮换和注册/登录限流；客户端在访问令牌到期前自动 refresh 并保存轮换后的令牌。退出经确认后调用 logout，撤销当前 Refresh Token 并清除本地会话，不影响其他登录会话；注销则永久删除账号及其关联数据。 |
| 单聊与同步 | WebSocket + Protobuf v2、持久化后 ACK、消息幂等、离线历史补拉和在线状态；批量恢复各联系人未读红点，可见会话批量已读，已读状态持久化并同步到双方连接。ACK 只表示服务端接收，不表示对方已读。 |
| 文件传输 | 默认 **5 MiB** 分块，逐块及整文件 SHA-256 校验；上传根据服务端已确认分块续传，支持暂停、继续、取消和重试。全系统按 **文件大小 + SHA-256** 复用已完成内容并秒传，最后一个逻辑引用删除后才清理对象。后台清理 24 小时无活动的上传，并重试失败的最终删除；最终清理会删除该内容前缀下的全部残留对象。下载暂停后继续会重新下载，当前不支持 Range。 |
| 公平调度 | 后端上传、Complete 和下载共享逐块公平队列，多文件竞争时按文件轮转，新文件不必等待整个大文件；单文件可利用空闲槽位。客户端固定 3 条上传分块通道和 3 条下载文件通道，上传按剩余量动态调度，下载由顺序 writer 流式原子落盘。 |
| 多用户客户端 | **一个进程对应一个已激活用户**；`--data-dir` 是共享 base，后端用户名决定 `<base>/users/<规范化用户名>/`。不同用户可共享 base，用户目录锁只拒绝同账号并发，会话、历史、配置和传输文件彼此隔离。 |
| 窗口与交互 | 普通登录页和聊天页固定为 **560×390**，联系人/聊天分割线不可拖动；ROOT 压测页可缩放并在切换时居中，返回后恢复固定尺寸。退出、注销及文件错误等提示框相对主窗口居中。 |
| ROOT 集成压测 | 正式客户端登录页集成“系统测试”和 ROOT 登录，必须向独立压测后端真实认证成功才能进入；支持自定义人数、80/18/2 自动负载分配、实时指标、历史报告和结论。ROOT 会话与普通会话隔离，独立 simulator 入口仍可使用。 |

后端默认最多 **96 个共享分块槽位**，同时受 **500 MiB 文件内存预算**约束。96 是槽位配置上限，500 MiB 是覆盖分块缓冲及额外开销的独立背压预算；实际并发还取决于排队工作，不能把两者理解为固定的“500 ÷ 5 = 100 路”，也不保证始终同时处理 96 个分块。

## 架构与目录

```text
secure-im-client / secure-im-simulator / secure-im-stress-runner
                            │ REST + WebSocket
                            ▼
                    Go 模块化单体后端
             正常实例 :8080 / 独立压测实例 :18080
                            │
                            ▼
             MySQL：业务元数据 / MinIO：明文分块
```

普通用户连接正常实例；ROOT 页面、simulator 和 stress runner 连接压测实例。两个后端共享 MySQL/MinIO 容器，但分别使用正常和压测数据库、Bucket。

后端 HTTP 层是**完整的 Gin 架构**：`cmd/server/router.go` 使用公开 `/api/v1` 路由组和认证子组，业务 Handler、认证、请求上下文、崩溃恢复与日志中间件均直接使用 `*gin.Context`。JSON 仍通过 `json.Encoder` 编码，文件流继续使用 `MaxBytesReader`、ResponseController deadline、流式分块和 `Content-Disposition`，Gorilla 直接执行 WebSocket Upgrade；对外 URL、JSON（含末尾换行）、状态码、请求 ID 和客户端协议保持不变。

| 路径 | 内容 |
|---|---|
| `backend/cmd/` | `server` 的应用装配、路由、健康检查、日志与 `stress-runner` 入口 |
| `backend/internal/` | 认证、聊天、文件传输、存储、配置和压测；各业务包拥有窄 repository，HTTP 错误只在边界映射 |
| `backend/start-local.ps1` | 读取本地环境、构建并启动正常后端 |
| `client/src/core/` | 协议、HTTP/WS、消息、传输、本地存储及 JSON 适配 |
| `client/src/app/` | 用户会话、目录迁移、ROOT 流程、压测进程、报告和评估；不依赖 Widgets |
| `client/src/ui/` | Qt Widgets 页面、展示组件、主题和交互协调 |
| `client/src/apps/` | 正式客户端和 simulator 的薄入口 |
| `client/tests/` | QtTest 契约、单元及可选真实后端集成测试 |
| `proto/im/v1/` | Protobuf v2 协议源文件，是 Go/Qt 两端共同源真相 |
| `deploy/` | 正常及独立压测 Compose 配置、本地环境文件 |
| `scripts/` | 环境检查、启动停止、协议生成和统一测试脚本 |
| `runtime/` | 本地用户目录和压测报告，均为运行期数据 |

## 快速启动

### 环境要求

- Windows PowerShell、Docker Desktop（含 Compose v2）、Go 1.27+。
- Qt 6.11.2 及 CMake、Ninja、llvm-mingw 工具链。随附 preset 固定使用 `D:/software/Qt` 下的本机路径；其他环境需调整 preset 或 CMake 配置。
- 客户端构建需要 Qt Protobuf/ProtobufTools。无需全局安装 protoc，首次构建可下载到 `client/.tools/`。

除特别说明外，以下命令均在项目根目录执行。

### 1. 启动并检查基础设施

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\dev-up.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\doctor.ps1
```

默认端口为 MySQL `13306`、MinIO API `19000`、MinIO 控制台 `19001`。首次启动会生成随机本地密码并写入被忽略的 `deploy/.env`；MinIO 登录凭据也在该文件中。

### 2. 启动正常 Go 后端

在独立 PowerShell 窗口执行并保持运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\backend\start-local.ps1
```

后端位于 `http://127.0.0.1:8080`。脚本会构建服务器、自动迁移数据库并检查 MinIO；首次生成的 JWT 密钥保存在本地环境文件中，供重启后继续使用。

### 3. 构建 Qt 客户端

CMake preset 位于 `client/CMakePresets.json`，必须先进入 `client` 再使用 preset：

```powershell
Set-Location client
.\tools\fetch_protoc.ps1
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
Set-Location ..
```

### 4. 启动两个独立用户

在两个 PowerShell 窗口分别执行以下命令；两个不同用户可以共享同一个 base：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
& .\client\build\qt-windows\bin\secure-im-client.exe --data-dir .\runtime --server-url http://127.0.0.1:8080 --username ALICE
```

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
& .\client\build\qt-windows\bin\secure-im-client.exe --data-dir .\runtime --server-url http://127.0.0.1:8080 --username BOB
```

启动后分别注册或登录即可互发消息和文件。普通用户数据实际位于 `.\runtime\users\ALICE\` 和 `.\runtime\users\BOB\`，不再使用 `u-` 前缀或启动别名嵌套。`--username` 可选，用于预填登录名及定向恢复匹配会话；不传时仅在存在唯一有效会话时自动恢复，多会话不会任意选择。旧 base 下的 `config/`、`downloads/` 支持带身份自动迁移或无身份首次登录询问迁移；`users/u-用户名` 以及历史 `alice/bob/users/u-用户名` 会在保存会话的用户 ID 和规范化用户名都匹配时整体 rename 到同级新目录。目标非空或存在多个来源时拒绝合并，旧目录锁被占用时停止，rename 失败保留原数据；历史容器只尝试 `rmdir`，非空时保留，绝不递归清理。

## ROOT 系统压力测试

### 1. 先启动独立压测后端

正常 MySQL/MinIO 必须已通过 `dev-up.ps1` 启动且健康，然后在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stress-up.ps1
```

脚本只启动独立压测后端 `http://127.0.0.1:18080`，复用正常基础设施容器，但使用独立数据库 `secure_im_stress` 和 Bucket `secure-im-stress-files`。它会注册并真实验证 **`ROOT` / `123456`**；该凭据仅限本地隔离压测。若 `/ready` 已就绪且 ROOT 真实登录、访问令牌验证成功，脚本直接返回，不重复写环境或构建。压测后端限制 6 CPU / 3 GiB，并关闭其注册/登录限流，不影响正常后端配置。

### 2. 从正式客户端进入并运行

启动 `secure-im-client` 后，点击登录页底部 **“系统测试”**，或输入 **`ROOT` / `123456`**（用户名大小写不敏感）。客户端先取得 `<项目>/runtime/system-test.lock`，再只执行 `stress-up.ps1` 环境准备，最后向 `18080` 发起真实 ROOT 登录；Setup 或认证失败会保留输出、停留在登录页并释放锁，可直接重试。正式客户端和 simulator 共用该锁，第二客户端占用时不会执行 Setup 或登录。普通账号仍登录 `--server-url` 指定的正常后端。

ROOT 状态保存在当前 `--data-dir` 下独立的 `root-stress/` 中，不会创建 `users/ROOT` 或嵌套进普通用户目录。进入压测页后选择总人数并点击 **“开始测试”**；“返回普通登录”会先停止并回收当前 runner，再释放系统测试锁并恢复普通页面，关闭窗口也遵循相同顺序。也可通过兼容入口启动独立工具：

```powershell
powershell -ExecutionPolicy Bypass -File .\client\start-simulator.ps1
```

默认负载为 1000 人、每秒建立 200 个连接、80% 挂机 / 18% 聊天 / 2% 文件、稳定阶段 5 分钟；文件用户传输 100 MiB 文件，采用 5 MiB 分块。runner 默认 400 个文件 worker，后端默认 96 个共享分块槽位和 500 MiB 文件内存预算，三者属于不同层级的限制。

### 3. 查看报告

每次运行写入一个独立目录，名称是 **UTC 时间戳加 6 位随机后缀**：

```text
runtime/stress-reports/YYYYMMDDTHHMMSSZ-xxxxxx/
例如：runtime/stress-reports/20261122T173605Z-eb183e/
```

目录包含 `report.html`、`summary.json`、`metrics.csv`、`config.json` 和 `conclusion.txt`。界面支持查看历史 HTML 或打开所在目录，并给出成功、波动或失败结论及瓶颈建议。`metrics.csv` 记录实际分块槽位使用量/上限；采样不可用时写为 `unknown`，不会伪造数值。

## 本地数据、离线资产与缓存

| 分类 | 路径或对象 | 处理原则 |
|---|---|---|
| 持久数据与本地密钥 | Docker 卷 `secure-im-mysql-data`、`secure-im-minio-data`；`runtime/users/`；`runtime/stress-reports/`；`backups/`；`deploy/.env`、`deploy/.env.stress` | 不是构建缓存。用户会话、历史、下载、报告、数据库和对象数据应按需备份；环境文件含凭据，不要提交或随意删除。压测数据与正常数据共用物理卷，但使用不同数据库和 Bucket。 |
| 离线工具与镜像资产 | `client/.tools/`、`backend/.tools/`；已拉取的固定版本 MySQL/MinIO Docker 镜像 | 离线环境中应保留。客户端工具可由 `fetch_protoc.ps1` 联网重新下载，镜像则需重新拉取；正常构建不需要全局 protoc。 |
| 可重建缓存与产物 | `backend/.cache/`、`backend/docker/secure-im-server`、`client/build/` | 可通过 Go/CMake 构建或启动脚本重新生成。清理后首次重建可能仍需网络取得 Go 模块、工具或镜像，离线时不要误删其依赖资产。 |

`stress-up.ps1` 在需要启动或重建时使用宿主机 Go 构建 Linux 后端，再通过 `Dockerfile.offline` 和已缓存的 MinIO 基础镜像制作压测镜像；已就绪快速路径不执行这些构建步骤。

### REF 源码参照

`backups/REF/` 保存重构前提交 `5cb97b7` 的 180 个受控源码与配置文件，未压缩且设为只读；`backups/REF.sha256.txt` 用于逐文件完整性核对。REF 不包含 `.git`、运行数据、真实 env、缓存、构建产物或工具二进制，也不是灾难恢复备份。后续开发不修改 REF；`backups/REF.safety.sha256.txt` 另行记录真实 env 和旧 ZIP 的防误改哈希。

### 协议再生成

修改 `proto/im/v1/*.proto` 后，使用仓内离线工具重新生成 Go 代码：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\generate-proto.ps1
```

脚本只更新 `backend/internal/proto/impb/` 中受版本控制的生成文件，不联网安装工具。Qt Protobuf 代码仍由 CMake 生成到 `client/build/`，不要手工编辑任何生成文件。

## 测试

在项目根目录运行统一测试脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test-all.ps1
```

默认依次执行 gofmt 检查、`go test ./...`、`go test -race ./...`、`go vet ./...`、Qt 配置/构建/CTest、全仓 PowerShell 语法检查，以及正常和压测两份 Compose 示例配置检查；任一步失败会立即停止。默认使用 `D:/software/Qt/Tools/CMake_64/bin/cmake.exe`，其他环境可指定：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test-all.ps1 -CMakePath "D:/path/to/cmake.exe"
```

可选真实后端集成测试需要先通过 `dev-up.ps1` 启动并确保 MySQL/MinIO 健康：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test-all.ps1 -Integration
```

`-Integration` 会在默认检查后启动独立压测后端，自动注入现有 Go 集成测试和 Qt `test_backend_integration` 所需环境，并在 `finally` 中执行 `stress-down.ps1` 清理后端；不会输出真实凭据，也不会启动大规模压测。

也可手动分步执行：

```powershell
Set-Location backend
go test ./...
go test -race ./...
go vet ./...

Set-Location ..\client
& "D:/software/Qt/Tools/CMake_64/bin/ctest.exe" --preset qt-windows
Set-Location ..
```

默认 Go 测试和 CTest 不需要手动启动压测服务。后端真实 API/MinIO 集成测试默认跳过，Qt `test_backend_integration` 也不注册到默认 CTest；环境变量和完整命令见详细文档：

- [后端配置、API 与集成测试](backend/README.md)
- [Qt 客户端、ROOT 压测与构建测试](client/README.md)

## 停止

先关闭客户端窗口，并在正常后端终端按 `Ctrl+C`，然后在项目根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stress-down.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1
```

`stress-down.ps1` 只移除独立压测后端，保留正常基础设施和所有数据。`dev-down.ps1` 默认保留数据卷；追加 `-RemoveVolumes` 会永久删除本地 MySQL/MinIO 卷，其中包括正常及压测数据，必须谨慎使用。
