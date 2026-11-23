# IMFT · 即时通讯与文件传输系统

IMFT 是一个面向本地开发、联调和负载验证的单聊桌面系统：Qt 客户端通过 REST 与 WebSocket 使用 Go 后端，MySQL 保存业务数据，MinIO 保存文件分块，并附带真实后端集成测试和 ROOT 系统压力测试。

如果你不仅想运行项目，还想按企业级项目的思路逐层看懂它，请继续阅读 [LEARN.md](LEARN.md)。

`secure-im-client`、`secure-im-server`、`secure-im-stress-*` 等仅是源码、可执行文件、容器、网络和存储资源的技术标识；产品名称统一为 **IMFT / 即时通讯与文件传输系统**。

## 1. 定位与安全边界

- 当前交付目标是可重复运行的本地开发环境，不是可直接暴露到公网的生产服务。示例地址均为回环地址，链路使用 HTTP，未内置 TLS 终止。
- 密码使用 Argon2id 哈希；受保护 API 使用 Bearer Access Token；WebSocket 使用短期、一次性 ticket。Access Token 默认 15 分钟，Refresh Token 默认 **30 天（720h）**，刷新时轮换。
- 客户端不会在令牌到期前主动刷新。只有**受保护请求收到 401** 后才触发刷新：同一会话的并发 401 采用 single-flight 共享一次刷新，成功后保存新令牌，并将等待中的原请求各重放一次；重放后再次 401 会使会话失效。
- **没有 E2EE**。消息内容不是端到端加密，MinIO 保存的也是上传接口收到的**明文分块**；没有 DEK/KEK 或应用层文件加解密。私有 Bucket 和参与者鉴权不等于内容加密。
- 在线连接 Hub 是后端**单实例内存实现**，不是集群在线状态或生产高可用方案。认证限流、WebSocket ticket 等进程内状态也不能替代多实例协调。
- ROOT / 123456 只用于本地隔离压测后端，不是正常环境的默认管理员账号。

## 2. 当前真实功能

- 账号注册、登录、会话恢复、退出登录和永久注销；按用户隔离本地会话、配置、消息与下载目录。
- 用户列表、在线状态、双人单聊、二进制 Protobuf v2 WebSocket 帧、持久化 ACK、幂等重发、离线历史增量同步、未读摘要和已读回执。
- 客户端持久化 outbox、断线恢复和有限重试；ACK 表示服务端已保存，不表示对方已读。
- 文件通知与分块上传/下载、逐块及整文件 SHA-256 校验、上传断点续传、暂停/继续/取消/重试、流式下载与原子落盘。
- 默认分块为 **5 MiB**，默认单文件上限为 **5 GiB**；已完成内容按 **`(size, sha256)`** 全局共享，相同内容可秒传，最后一个逻辑引用删除后才清理对象。
- 普通客户端一个进程激活一个用户，可多开进程使用不同用户；同一客户端内提供 ROOT 系统测试页，展示在线、消息、文件、时延、资源指标和历史报告。

## 3. 组件与源码地图

```text
secure-im-client ── REST / WebSocket ──> 正常 Go/Gin 后端 :8080
                                              ├─ MySQL：secure_im
                                              └─ MinIO：secure-im-files（明文分块）

ROOT 系统测试页 ──> secure-im-stress-runner ──> 压测后端 :18080
                                              ├─ MySQL：secure_im_stress
                                              └─ MinIO：secure-im-stress-files
```

正常后端在宿主机运行；正常 Compose **只有 MySQL 和 MinIO**。压测 Compose 只增加独立后端容器，复用正常环境的 MySQL/MinIO 容器及物理卷，但使用不同数据库和 Bucket。

| 路径 | 职责 |
|---|---|
| `backend/cmd/server/` | Go 1.27+ / Gin 后端的装配、路由、健康检查和生命周期 |
| `backend/cmd/stress-runner/` | 真实负载生成器入口 |
| `backend/internal/` | 认证、单聊、账号、文件、存储、配置及压测实现 |
| `backend/start-local.ps1` | 注入本地环境、构建并启动正常后端 |
| `client/src/core/` | Qt 网络、协议、消息同步、本地存储和文件传输 |
| `client/src/app/` | 用户会话、ROOT 流程、runner 进程与报告逻辑 |
| `client/src/ui/`、`client/src/apps/` | Qt 6 Widgets 界面及程序入口 |
| `client/tests/` | QtTest/CTest 与真实后端集成测试 |
| `proto/im/v1/` | WebSocket Envelope 与业务 payload 的协议源文件 |
| `deploy/` | 正常/压测 Compose 与环境示例 |
| `scripts/` | 基础设施、压测后端、诊断和测试脚本 |

客户端使用 C++17、Qt Widgets/Network/WebSockets/Test/Protobuf/ProtobufTools。Go 生成代码已入库，正常 Go 构建不需要 `protoc`；Qt 构建需要宿主机 `protoc` 驱动代码生成。

## 4. 环境要求

以下命令以 Windows PowerShell 和仓库根目录为基准：

- Go **1.27+**，以及支持 CGO 竞态检测的本机工具链。
- Docker Desktop（含 Docker Compose v2），用于 MySQL 8.4.6、MinIO 和压测后端。
- Qt **6.11.2 llvm-mingw**、Ninja、CMake 3.25+；当前 preset 固定使用 `D:/software/Qt/6.11.2/llvm-mingw_64` 和 Qt 自带 llvm-mingw 编译器。
- CMake 默认路径为 `D:/software/Qt/Tools/CMake_64/bin/cmake.exe`；测试脚本可用 `-CMakePath` 覆盖。Qt 安装位置不同时需相应调整本机 preset。
- 首次 Qt 配置前运行 `client/tools/fetch_protoc.ps1`，将 protoc 29.3 下载到 `client/.tools/`；此步骤需要网络，不做全局安装。

## 5. 启动顺序：基础设施 → 正常后端 → 客户端

### 5.1 启动并检查基础设施

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\dev-up.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\doctor.ps1
```

`dev-up.ps1` 等待 MySQL/MinIO 健康并创建、验证私有 Bucket `secure-im-files`。默认端口为 MySQL `13306`、MinIO API `19000`、Console `19001`。

### 5.2 启动正常后端

保持该终端运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\backend\start-local.ps1
```

脚本构建宿主机后端并监听 `http://127.0.0.1:8080`。启动时会检查 MySQL、执行迁移并确保 MinIO Bucket 存在；`/health` 是存活检查，`/ready` 检查数据库和对象存储。

### 5.3 构建并启动 Qt 客户端

另开终端，在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\client\tools\fetch_protoc.ps1
Push-Location client
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
Pop-Location

$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
& .\client\build\qt-windows\bin\secure-im-client.exe `
  --data-dir .\runtime --server-url http://127.0.0.1:8080 --username ALICE
```

`--data-dir` 必填，`--username` 仅用于预填和定向恢复会话。需要第二个用户时，在另一终端设置同样的 Qt `PATH`，将最后的用户名改为 `BOB` 再启动，然后分别注册或登录。

## 6. ROOT runner 与系统测试

先完成基础设施启动和 Qt 构建，再**单独构建真实 runner**：

```powershell
go -C backend build -o ../client/build/qt-windows/bin/secure-im-stress-runner.exe ./cmd/stress-runner
```

普通 Qt 配置/构建**不会生成** `secure-im-stress-runner.exe`；缺少它时，界面可以构建成功，但真正开始系统测试会无法启动负载进程。

使用正常方式启动 `secure-im-client`，点击登录页的“系统测试”，或输入 `ROOT / 123456`。进入过程为：

1. 获取 `runtime/system-test.lock`，防止多个客户端同时准备或压测。
2. 自动执行 `scripts/stress-up.ps1`，要求正常 MySQL/MinIO 已健康；脚本创建压测凭据、独立数据库和 Bucket，构建并启动 `127.0.0.1:18080` 的压测后端，再验证 ROOT 真实登录。
3. 登录成功后显示系统测试页。**进入 ROOT 只做环境准备，不启动 runner**；选择人数并点击“开始测试”后，客户端才再次检查环境并启动 runner。

也可手动准备或停止压测后端：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\stress-up.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\stress-down.ps1
```

默认负载为 1000 人、80/18/2 的空闲/聊天/文件比例、5 分钟稳定期；文件任务使用 100 MiB 文件和 5 MiB 分块。压测后端限制为 6 CPU / 3 GiB。报告位于 `runtime/stress-reports/<run-id>/`，包括 `summary.json`、`metrics.csv`、`report.html` 等。`stress-down.ps1` 只移除压测后端，不停止正常基础设施，也不删除数据卷。

## 7. 测试与业务冒烟

### 默认检查

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-all.ps1
```

默认流程执行 gofmt、Go 单元测试、Go 竞态检测、`go vet`、Qt 配置/构建、默认 CTest、PowerShell 语法检查以及两套 Compose 示例配置检查；不会启动真实后端，`test_backend_integration` 不注册到默认 CTest。CMake 不在默认位置时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-all.ps1 -CMakePath "D:/path/to/cmake.exe"
```

### 真实后端集成测试

先运行 `dev-up.ps1`，再执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-all.ps1 -Integration
```

`-Integration` 包含全部默认检查，并额外启动隔离压测后端，在 `http://127.0.0.1:18080` 运行真实 Go `TestReal*` 和 Qt `test_backend_integration`，结束后清理压测后端；正常基础设施及数据卷保留。它不是大规模负载测试。

### 正常环境业务冒烟

正常后端已启动且 Qt 已构建时，可显式运行未加入默认 CTest 的端到端用例：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:CLIENT_BACKEND_URL = "http://127.0.0.1:8080"
& .\client\build\qt-windows\test_backend_integration.exe
Remove-Item Env:CLIENT_BACKEND_URL
```

该用例创建随机双用户，覆盖注册/登录、实时消息、重复帧 ACK、已读、文件上传/下载和账号注销。它会真实写入正常环境并在末尾注销测试账号，不应指向含有重要数据的外部共享服务。

## 8. 环境文件、运行数据与历史参照

### `.env` 与凭据

- 脚本优先读取根目录 `.env`，其次读取 `deploy/.env`。两者都不存在时，`dev-up.ps1` 根据 `deploy/.env.example` 创建 `deploy/.env` 并生成随机本地密码。
- `backend/start-local.ps1` 从上述文件组装进程环境，并在缺少时追加持久化的随机 `JWT_SECRET`。**Go 服务本身不会自动加载 `.env`**；不通过该脚本启动时，应参考 `backend/.env.example` 自行导出变量。
- `stress-up.ps1` 生成被忽略的 `deploy/.env.stress`，保存压测专用 MySQL/JWT 凭据。环境文件包含秘密，不应提交、公开或当作普通缓存删除。

### `runtime/` 与持久化数据

- `runtime/users/<用户名>/` 保存各普通用户的会话、历史、配置和下载等本地数据；ROOT 使用独立的 `runtime/root-stress/`（以 `runtime` 为 data base 时）。
- `runtime/system-test.lock` 是系统测试互斥锁，`runtime/stress-reports/` 是报告目录。整个 `runtime/` 都是运行期数据，不是源码。
- Docker 命名卷 `secure-im-mysql-data`、`secure-im-minio-data` 同时承载正常和压测数据；停止容器不会自动删除它们。正常与压测的隔离是数据库/Bucket 级隔离，不是物理资源隔离。

### `backups/REF/`

`backups/REF/` 是提交 `5cb97b73b2e5e06df95e63f20b038f5ddc938978` 的 180 个只读源码/配置参照文件，不是当前源码，也不是灾难恢复备份。`backups/REF.sha256.txt` 用于逐文件完整性核对；`backups/REF.safety.sha256.txt` 另记录真实环境文件和历史 ZIP 的防误改哈希。开发、构建和测试均不应修改或执行 REF 中的内容。`backups/` 整体属于本地忽略数据。

## 9. 停止与危险操作

```powershell
# 先停止独立压测后端，保留基础设施和全部数据
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\stress-down.ps1

# 停止 MySQL/MinIO，默认保留命名卷
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1
```

**危险：**下面的 `-RemoveVolumes` 会永久删除 `secure-im-mysql-data` 和 `secure-im-minio-data`，其中同时包含正常及压测数据库、文件对象，无法通过停止操作恢复：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1 -RemoveVolumes
```

## 10. 当前限制

- 仅支持双人单聊和 Qt Widgets 桌面客户端；没有群聊、QML 或生产级公网部署编排。
- Hub 为单实例内存连接表，重启会断开实时连接；离线消息靠数据库历史补拉。当前实现不提供多实例 presence、分布式限流或高可用。
- 无 E2EE，MinIO 分块为明文；默认 HTTP 配置需要由可信回环边界保护，不能直接视为公网安全方案。
- 上传可依据服务端已确认分块续传；下载接口没有 Range，下载暂停后继续会重新获取完整文件。
- 全局 `(size, sha256)` 内容复用是存储去重机制，不是按用户隔离的加密或隐私承诺。
- 压测环境共享正常 MySQL/MinIO 的物理资源，报告反映的是本机与该配置下的结果，不构成生产容量或高可用证明。

## 11. 项目亮点介绍

IMFT 的亮点不只是“聊天和文件功能都能运行”，而是把断线、重复、并发、资源不足和异常中断也纳入了设计。下面这些机制共同构成了项目的工程价值。

### 11.1 分块公平调度：像时间片轮转一样照顾每个文件

后端的上传、完成校验和下载共享一个分块调度器。多个文件同时竞争时，同一文件最多占一个活动槽位，刚处理过的文件会让出下一轮，效果类似操作系统的时间片轮转：大文件不能一直霸占通道，小文件和新文件也能得到处理。

如果当前只有一个文件，它又可以使用多个空闲槽位，不会为了形式上的公平浪费资源。默认 96 个槽位只是上限，实际并发还受工作量和内存预算约束。

### 11.2 消息 outbox 与幂等：断线后可以重发，但不会轻易重复

客户端先把发送意图、固定 `message_id` 和内容写入本地 outbox，再写 WebSocket。发送失败或重启后仍能恢复，而不是只依赖内存里的一句“已经点了发送”。

服务端通过唯一约束和业务幂等识别重复消息：同一发送者、同一 ID、同一内容返回原来的 ACK/cursor；同一 ID 换成不同内容则明确拒绝。ACK 只表示服务端已经保存，READ 才表示对方已经阅读，两种状态不会混为一谈。

### 11.3 认证竞态处理：很多请求失败，也只刷新一次

多个请求同时收到 401 时，客户端通过 single-flight 共享一次 Refresh，而不是同时消费同一个 Refresh Token。刷新成功后，等待中的原请求各重放一次；认证 epoch 和稳定请求句柄还会阻止旧会话的迟到回调覆盖新登录或新网络请求。

这解决的是实际系统中很容易出现的“请求都成功了，令牌却被另一个线程覆盖”的隐蔽问题。

### 11.4 文件秒传与引用管理：共享内容，但不误删别人的文件

文件内容用 `(size, SHA-256)` 标识。相同内容已经完成时，新发送只增加一个逻辑引用并立即完成，不必重复上传和保存相同字节。

每个逻辑引用可以独立删除，只有最后一个引用消失后才清理 MinIO 对象。最终删除采用“数据库屏障事务 → 事务外清理对象 → 数据库完成事务”的两阶段流程，失败会保留可重试状态，避免把外部对象操作误当成 MySQL 事务的一部分。

### 11.5 背压与资源预算：处理不过来时，不允许内存无限增长

客户端下载使用有界写队列，队列达到高水位就暂停网络读取，降到半水位后恢复；单文件顺序写入并通过 `QSaveFile` 原子提交，避免慢磁盘导致内存堆积或半成品被当成完整文件。

后端同时限制共享分块槽位和加权内存预算，客户端另有 3 条上传分块通道、3 条下载文件通道及公共线程池。它们分别约束不同资源，不能简单用 `500 MiB ÷ 5 MiB` 推断固定并发数。

### 11.6 用户数据隔离与安全迁移：换目录结构，不拿旧数据冒险

普通用户的数据统一放在 `<base>/users/<规范化用户名>/`，会话、历史、下载和目录锁彼此独立。`--data-dir` 只是共享 base，真正的数据归属由登录用户名决定。

旧目录迁移会同时核对用户 ID 和规范化用户名，目标非空、多来源或旧锁占用都会停止；采用整体 rename，不做不受控的非空合并，失败时保留原始数据。

### 11.7 清晰分层：把“怎么展示”“怎么处理”“怎么保存”分开

后端由 `cmd/server` 统一装配，业务包拥有自己的 Service 和窄 repository，业务错误只在 `httpx` 边界转换成 HTTP 响应。客户端则按 `core → app → ui` 组织，Widget 不直接解析后端 JSON。

这种分层不是单纯增加文件夹，而是让协议适配、业务规则、持久化和界面可以分别理解、测试和修改，降低局部调整牵一发动全身的风险。

### 11.8 可恢复的后台清理：异常任务不会永远卡在半路

闲置上传和最终删除由独立清理器周期处理。候选任务会在执行前重新检查状态和时间条件，共享内容仍被引用时不会误删；失败任务保留状态并在后续轮次重试，清理轮次串行执行且有时间边界。

这让“进程曾经崩溃”或“MinIO 曾经不可用”不必依赖人工记住每个半成品。

### 11.9 验证体系：不仅证明能编译，还证明组件真的协作

项目同时具备 Go 单元测试、竞态检测、静态检查、Qt 契约与界面测试、真实 MySQL/MinIO 集成测试、业务冒烟和 ROOT 压测。不同测试回答不同问题，不用一次界面操作代替全部质量保证。

压测报告记录配置、指标时间序列和结论；资源采样不可用时明确写 `unknown`，不会为了报告好看而编造数值。统一测试入口和 REF 只读快照也使重构可以边改边验证、随时对照。
