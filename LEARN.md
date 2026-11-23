# IMFT：从“会写 Demo”到“看得懂企业级项目”

IMFT（即时通讯与文件传输系统）这份指南不是 Go、Qt 或 Protobuf 语法课。默认你会 Python/C++/FastAPI/SQL，遇到 Go 可以借助 AI 看懂。我们要补的是另一层能力：**看到一堆目录时知道先看哪里，看到一次请求时知道它穿过哪些组件，看到故障时知道系统为什么不会丢数据、重复执行或把内存打爆。**

建议不要从第一个文件顺序读到最后一个文件。读企业项目像查案：先画地图，再沿三条主线追踪——**请求怎么走、状态存在哪里、失败怎么办**。

## 1. 为什么 Demo 能跑，不等于项目完整

Demo 往往只证明“开心路径”成立：点击发送，消息出现；选择文件，上传成功。完整项目还必须回答下面的问题：

| Demo 里容易忽略的问题 | IMFT 给出的工程答案 |
|---|---|
| 请求发出后断网，到底成功没有？ | 消息使用持久化 outbox、固定 ID、ACK 和历史对账，不靠“界面上发出去了”猜结果。 |
| 用户连续点击、客户端重试，会不会产生两条记录？ | 数据库唯一约束加业务幂等，同一消息重复提交返回原结果。 |
| 同一客户端多个请求同时遇到 401，会不会互相顶掉？ | 客户端合并并发刷新，服务端原子轮换 Refresh Token。 |
| 上传了一半，重启后要从零开始吗？ | 服务端记录已确认分块，客户端续传；完成前还要校验整文件哈希。 |
| 两个人发同一文件，要保存两份吗？删一份会怎样？ | 内容按 `size + SHA-256` 复用，逻辑引用与物理内容分开，最后一个引用删除后才清对象。 |
| 一个慢用户不读数据，会不会拖死所有人？ | 有界队列、背压、超时和慢连接断开，不能无限往内存里塞。 |
| 代码升级后，旧数据库和旧客户端怎么办？ | 顺序迁移、协议源文件、兼容边界和真实集成测试。 |
| 服务返回 200，就代表依赖都可用吗？ | `/health` 只看进程活着，`/ready` 还检查 MySQL 和 MinIO。 |
| 1000 个用户来了，多少并发算合理？ | 显式连接数、分块槽位、内存预算和压测报告，而不是“机器应该扛得住”。 |

所以，“企业级”不是目录多、术语多，也不是一定要微服务。IMFT 后端是**模块化单体**，但它认真处理了失败、并发、持久化、资源边界、升级和验证。先学会这些，比急着拆十个服务重要得多。

## 2. 先把整个系统串起来

可以把一次使用过程想成下面这张图：

```text
Qt Widgets 界面（client/src/ui）
        │ 用户操作，不直接拼后端 JSON
        ▼
应用逻辑（client/src/app：会话、目录、ROOT 压测）
        │
        ▼
客户端核心（client/src/core）
  REST/JSON：登录、历史、已读、文件元数据与文件字节
  WebSocket/二进制 Protobuf：消息、ACK、ERROR、READ、心跳
        │
        ▼
Go 模块化单体（backend/cmd/server 装配，backend/internal 实现）
        ├── MySQL：用户、令牌、会话、消息、文件元数据、审计、迁移版本
        ├── MinIO：文件的实际明文分块
        └── 内存 Hub：当前进程的在线 WebSocket 连接与实时推送
```

这里最容易混淆的是 **MySQL 和 MinIO 的分工**：

- MySQL 回答“这个用户是谁”“这条消息属于谁”“这个文件有哪些分块、传到哪了、谁可以下载”。
- MinIO 回答“文件第 N 块的字节放在哪里”。文件内容不走 JSON，也不 Base64 塞进聊天消息。
- 文件上传完成后，聊天里发送的是带 `file_id` 的文件通知；接收方再凭文件权限走下载接口。聊天通道和文件字节通道相互配合，但不是一回事。
- `proto/im/v1/*.proto` 是 Go 与 Qt 两端共同的实时协议约定。它不是“Go 的私有格式”，而是两端契约的源真相。

还要记住当前边界：在线 Hub、部分锁和限流状态在单进程内存中；MinIO 中保存的是明文分块；本地默认 HTTP/WS 只绑定回环地址。**这个项目适合学习真实工程机制，不代表已经具备多节点容灾、公网 TLS 或端到端加密。**

## 3. 目录地图：哪些该读，哪些先跳过

### 3.1 源码主线

| 路径 | 你先把它理解成什么 |
|---|---|
| `backend/cmd/server/` | 后端“总装车间”：启动、依赖装配、路由、健康检查、日志和退出生命周期。 |
| `backend/cmd/stress-runner/` | 独立负载发生器，不是正常聊天后端的一部分。 |
| `backend/internal/config/` | 从进程环境变量构造并校验配置。 |
| `backend/internal/store/` | 数据库连接池、嵌入式 SQL 迁移和审计。 |
| `backend/internal/auth/` | 注册登录、密码哈希、JWT、刷新轮换、退出和认证限流。 |
| `backend/internal/chat/` | WebSocket、消息持久化、ACK、历史、未读、READ、在线 Hub。 |
| `backend/internal/filetransfer/` | 文件上传、秒传、续传、完成校验、下载、删除、调度、内存预算和清理。 |
| `backend/internal/account/` | 注销账号时跨消息、文件、令牌和连接的协调。 |
| `backend/internal/apperror/`、`httpx/` | 业务错误与 HTTP 错误之间的边界。 |
| `backend/internal/stress/` | 压测阶段、指标、评估、报告和清理。 |
| `client/src/core/` | 网络、协议适配、本地持久化、可靠消息和文件传输，不负责画窗口。 |
| `client/src/app/` | 用户激活、目录隔离与迁移、ROOT 会话、runner 进程和报告逻辑；不依赖 Widgets。 |
| `client/src/ui/` | 页面、窗口、交互协调和展示。 |
| `client/src/apps/` | 正式客户端与 simulator 的薄入口。 |
| `proto/im/v1/` | WebSocket Protobuf v2 的手写契约。 |
| `deploy/` | 正常基础设施和独立压测后端的 Compose 配置，以及本地环境文件。 |
| `scripts/` | 启动、停止、检查、协议生成和统一测试的自动化入口。 |
| `runtime/` | 程序运行后产生的数据，不是源码。 |

### 3.2 第一遍不必读的内容

- `backend/internal/proto/impb/*.pb.go`：由 proto 生成的 Go 代码，读 `.proto` 更清楚。
- `client/build/`：CMake、Qt Protobuf 生成代码、测试程序和 exe，全部可重建。
- `backend/.cache/`：Go 模块与构建缓存；`backend/docker/secure-im-server` 也是构建出的 Linux 二进制。
- `client/.tools/`、`backend/.tools/`：本地生成/构建工具，不是业务实现；离线环境不要为了“清缓存”随手删除。
- `runtime/users/`、`runtime/stress-reports/`：排查本地数据或报告时有价值，学习架构时不用逐文件读。
- `backups/REF/`：只读历史参照，不参与编译和运行，不要把它当成第二套当前源码。
- Docker 的 MySQL/MinIO 数据卷：是持久数据，不应通过“去容器目录里读源码”的方式学习项目。

一句话：**手写契约和业务源码优先，生成物看输入与生成规则，缓存只在构建出问题时看，运行数据只在状态出问题时看。**

## 4. 推荐核心阅读顺序：每一步只回答几个问题

不要一上来扎进最复杂的文件传输。按下面顺序读，每一步读完能用自己的话回答“阅读目的”即可。

### 第一站：路由与装配——服务是怎样被拼出来的

1. `backend/cmd/server/routes.go`
   - 看公开路由和需要认证的路由怎样分组。
   - 建立“URL 属于 auth、chat、account 还是 filetransfer”的索引，不必背 API 字段。
2. `backend/cmd/server/router.go`
   - 看 Gin 中间件、认证子组和 `/ws` 的特殊分发。
   - 理解路由层负责交通，不负责具体业务。
3. `backend/cmd/server/app.go`
   - 看启动顺序：配置 → MySQL → 迁移 → MinIO → 业务服务 → 后台清理器 → HTTP。
   - 看退出时如何关闭连接、停止清理器、释放数据库。
4. `backend/cmd/server/main.go`、`health.go`、`logging.go`
   - 分清进程入口、健康检查和横切日志分别放在哪里。

这一步最重要的词是 **composition root（组合根）**：`cmd/server` 知道所有具体实现并把它们接起来，业务包不必互相到处 new 对方。

### 第二站：配置与数据库——程序启动前必须相信什么

- `backend/internal/config/config.go`：看默认值、环境变量读取和范围校验。重点不是每个变量，而是“非法配置为什么应该在启动时暴露”。
- `backend/internal/store/db.go`：看连接池限制、嵌入式迁移、`schema_migrations` 防重和 MySQL `GET_LOCK` 防并发升级。
- `backend/internal/store/migrations/001_init.sql`：先找用户、消息唯一键和文件表，不必背全部 DDL。
- `003_shared_plaintext_files.sql`：看文件模型为什么从单次上传演进为“逻辑引用 + 共享内容”。
- 再快速看 `002_messages_read_at.sql`、`004_file_cleanup_index.sql`，理解功能和索引都可以逐步演进。

### 第三站：认证——身份怎样跨请求延续

按 `service.go` → `token.go` → `refresh.go` → `logout.go` → `repository.go` → `http.go` 阅读 `backend/internal/auth/`。再补 `password.go` 和 `rate_limit.go`。

阅读目的：

- 分清密码哈希、短期 Access Token、长期 Refresh Token 各自解决什么。
- 找出 Refresh Token 为什么只存哈希、为什么每次刷新都轮换，以及并发轮换怎样保证只有一个成功。
- 看认证中间件怎样把用户 ID 放进请求上下文，业务 Handler 怎样避免重新解析 JWT。

### 第四站：聊天——“收到”到底有哪几种含义

建议顺序：

1. `proto/im/v1/envelope.proto` 和 `messages.proto`：先认 `MESSAGE/ACK/ERROR/READ`、`message_id` 和 `cursor`。
2. `backend/internal/chat/http.go`：跟一帧从 ticket 校验、WebSocket 升级到保存、推送、ACK 的路径。
3. `service.go`、`repository.go`：看消息校验、会话创建、唯一键幂等和事务。
4. `hub.go`：看在线连接表、每用户连接上限、有界发送队列和慢消费者处理。
5. `history.go`、`read.go`、`unread.go`、`ticket.go`：补齐离线恢复、已读和一次性连接票据。

### 第五站：文件传输——把“内容”和“这次发送”分开

在 `backend/internal/filetransfer/` 中按下面顺序读：

- `model.go`：先看 `File` 是逻辑引用，`ContentID` 指向可共享内容；再看权限与分块边界。
- `service.go`、`repository.go`：看状态与 SQL/事务边界。
- `upload.go` → `complete.go` → `download.go`：跟上传、整文件校验和流式下载。
- `objectstore.go`：看 MinIO 被窄接口隔离在哪里。
- `deletion.go`、`cleanup.go`：看最后一个引用删除与失败重试。
- `scheduler.go`、`memory.go`：看公平分块调度和内存预算，理解它们不是简单的“全局开 96 个 goroutine”。

不要试图一次记住所有状态。画一张 `uploading → ready/completed → deleting` 的纸面状态图，比逐行翻译更有用。

### 第六站：客户端 core → app → ui——界面之外还有一整套系统

先读 core：

- `client/src/core/ApiClient.h`：稳定公共入口和认证 epoch。
- `ApiClientTransport.cpp`、`ApiClientRefreshLogout.cpp`：401 后刷新与原请求重放。
- `ApiClientEndpoints.cpp`、`ApiClientHistory.cpp`、`ApiJsonCodec.*`：REST 字段适配，注意 UI 不直接依赖 JSON。
- `WebSocketClient.*`、`WireCodec.*`、`ProtobufWireCodec.*`、`MessagePayloadCodec.*`：从网络字节到业务消息。
- `MessageDelivery.*`、`LocalStore.*`、`LocalStoreMessageCodec.*`：outbox、重试、ACK、历史对账和本地持久化。
- `TransferManager.*`、`TransferUpload.cpp`、`TransferDownload.cpp`、`TransferScheduling.cpp`、`Workers.*`：客户端 3+3 通道、续传、背压和原子落盘。

再读 app：

- `SessionContext.*`：为什么一个进程只激活一个普通用户，`--data-dir` 为什么只是共享 base，以及用户锁和旧目录迁移。
- `RootStressSession.*`：ROOT 为什么与普通会话隔离。
- `StressRunController.*`、`StressNdjsonCodec.*`、`StressReportRepository.*`、`StressAssessment.*`：环境准备、runner、输出协议、历史报告和结论如何分层。

最后读 ui：

- `client/src/apps/client_main.cpp` 是入口。
- `SessionPage.*`、`SingleUserWindow.*` 管页面与窗口生命周期。
- `MainWindow*.cpp` 把联系人、聊天、READ、文件任务接到界面上。
- `SystemTestPage.*` 只负责压测交互展示，不亲自实现负载算法。

读 UI 时始终追问：“这个点击最终调用了 core/app 的哪个动作？”不要把精力放在每个控件怎么摆。

### 第七站：测试与压测——看作者怎样证明前面的承诺

- 后端包旁的 `*_test.go`：先读与刚学模块对应的测试，测试往往比实现更直接地写出规则。
- `backend/integration*_test.go`：看真实 MySQL/MinIO/HTTP/WebSocket 的端到端承诺。
- `client/tests/`：优先看 `test_message_delivery.cpp`、`test_transfer_manager.cpp`、`test_api_client.cpp`、`test_message_sync.cpp`，再看 UI、目录迁移和 ROOT 测试。
- `backend/internal/stress/` 与 `backend/cmd/stress-runner/`：最后学习负载阶段、资源采样、报告与清理。
- `client/src/app/Stress*`、`client/tests/test_stress_run.cpp`：看 Qt 怎样安全地管理外部 runner，而不是把压测塞进 UI 线程。

## 5. 三条核心流程：把正常路径和失败路径一起看懂

### 5.1 认证：401 不是简单“再登录一次”

正常路径是：注册时密码经 Argon2id 哈希；登录后拿到短期 Access Token 和长期 Refresh Token；Access Token 放在受保护请求的 Bearer 头里。WebSocket 不直接长期使用 Access Token，而是先换一次性、短期 ticket，再 Upgrade。

关键看 Access Token 过期后的流程：

1. 某个受保护请求收到 401。
2. 客户端把原请求挂起，并发起 refresh。多个并发 401 共享同一次刷新，不能同时消费同一个 Refresh Token。
3. 服务端原子轮换：旧 Refresh Token 失效，返回新的 Access/Refresh 双令牌。数据库中只保存 Refresh Token 的哈希。
4. 客户端先持久化新会话，再让等待中的**原请求各重放一次**，并自动带上新 Access Token。
5. 如果重放仍然 401，或刷新失败，就清除失效会话、回登录页，不进行无限刷新循环。
6. 登录、退出等认证边界会推进 epoch。旧会话迟到的刷新回调不能覆盖新会话。

这就是“刷新重放”完整含义：**不是只刷新令牌，而是安全地恢复刚才失败的业务请求。** 也不要把它推广成“所有错误都重试”：401 有专门且只有一次的重放规则，业务拒绝、参数错误不该照抄。

### 5.2 消息：outbox、幂等、ACK 和 READ 各管一件事

发送文本或文件通知时：

1. 客户端先生成固定 `message_id`、payload 和时间戳，把待发送消息写入本地持久化 outbox，然后才写 WebSocket。这样即使第一次网络写入失败，也有一条可恢复的“发送意图”。
2. 服务端校验 Protobuf v2，创建/找到单聊会话，先把消息持久化到 MySQL，再向接收者在线连接推送，并向发送者返回 ACK。
3. MySQL 的 `(sender_id, message_id)` 唯一约束兜底。同一发送者用同一 ID、同一内容重试，返回原 ACK/cursor，不再次推送；同 ID 换内容则冲突拒绝。
4. 客户端约 12 秒没收到 ACK 会指数退避重试，最多接受 3 次完整帧写入。断线本身不消耗次数；ACK 丢失或迟到时，历史里出现该消息也能证明服务端已经接受，用来对账。
5. 接收方不在线时，Hub 不强撑“实时必达”；重连后按数字 cursor 拉历史。Hub 的慢连接队列有上限，占满会断开，离线历史仍是恢复手段。

**ACK ≠ READ：**

- ACK：服务端已持久化这条消息，解决“发送是否被接收”。
- READ：接收者已经阅读到某个 cursor，解决“对方看到哪里”。
- 已读由独立 REST 请求持久化，随后通过 WebSocket `READ` 通知双方连接；`READ` 事件本身不作为聊天消息再存一份。

所以“已 ACK 未读”是完全正常的中间状态，不是 bug。

### 5.3 文件：秒传、分块和引用删除是一套内容管理模型

上传主线：

1. 客户端先算整文件 SHA-256，用文件名、大小、哈希、接收者等调用 init。
2. 服务端用 **`size + SHA-256`** 查找已 ready 的共享内容。命中时创建新的逻辑文件引用并返回 `instant=true`，客户端跳过上传和 complete，这就是秒传；只存在尚未完成的相同上传不算秒传。
3. 未命中时按默认 5 MiB 分块上传原始字节，每块带自己的 SHA-256。状态接口返回服务端已经确认的 `uploaded_chunks`，暂停或重连后跳过这些块。
4. complete 时服务端重新读取分块，校验逐块及整文件哈希，全部通过才把共享内容标为 ready。上传接口返回成功不等于整文件已经可下载。
5. 文件通知通过消息系统发送 `file_id`；下载时校验发送者/接收者权限，以流式方式读出。当前下载暂停后继续会重新下载整个文件，不要误以为已经支持 HTTP Range。

删除主线更能体现企业思维：

- 每次 init 得到的是一个**逻辑引用**，多个引用可以指向同一份物理内容。删除一个引用，其他引用仍能下载。
- 只有最后一个引用删除，才清理 MinIO 中该内容前缀下的对象，包括替换分块可能留下的残留对象。
- 删除采用“数据库屏障事务 → 事务外清理对象 → 数据库完成事务”的两阶段协议。外部 MinIO 调用不塞进数据库长事务；中途失败会保留可重试的 deleting 状态，后台清理器之后接着做。
- 正在等待最终清理时，不允许相同内容悄悄挂上新引用，否则会出现“旧清理任务把新引用需要的数据删掉”。

## 6. 测试为什么这样放，以及四种测试分别在证明什么

### 6.1 放置位置不是随便选的

- **Go 包旁的 `*_test.go`**：Go 工具天然按包组织测试。测试与实现相邻，改 `chat` 就能立刻看到消息、Hub、历史等规则；需要时也能验证包内细节。
- **`client/tests/`**：C++/Qt 测试要作为独立 QtTest 可执行文件，由 CMake 链接 `client_core`、`client_application` 或 `client_ui`，再交给 CTest。集中放置让构建目标和测试依赖更清楚，不意味着测试离业务很远。
- **真实集成默认不自动跑**：Go 的 `backend/integration*_test.go` 默认跳过；`client/tests/test_backend_integration.cpp` 虽会构建成 exe，但在 `client/CMakeLists.txt` 中用 `NO_CTEST` 排除默认注册。它们需要正在运行的后端、MySQL、MinIO 和凭据，还会创建真实数据，不能假设每台开发机执行 `go test` 或 CTest 时都具备这些条件。

`test-all.ps1 -Integration` 是显式入口：准备隔离压测后端、注入环境、执行真实 Go/Qt 集成测试，最后清理后端。它不是默认单元测试的替代品。

### 6.2 单元、集成、冒烟、压测不要混为一谈

| 类型 | 回答的问题 | 本项目例子 |
|---|---|---|
| 单元测试 | 这一小段规则在可控输入下对不对？ | 哈希、token、消息幂等、分块边界、outbox、JSON 编解码、调度器。通常用假依赖或临时目录，快且可重复。 |
| 集成测试 | 多个真实组件接起来后，契约还成立吗？ | 真实后端 + MySQL + MinIO，双用户发消息、READ、历史、上传下载、秒传、注销。 |
| 冒烟测试 | 这次部署/启动是不是基本能用，值得继续测吗？ | 先查 health/ready，再跑一小段真实注册、聊天和文件流程。范围小，目标是尽早发现“根本不能用”。 |
| 压力测试 | 在规定负载与资源预算下，吞吐、延迟、稳定性是否达标？ | ROOT runner 的 80/18/2 负载、稳定阶段、指标采样和报告。不是“多开几个客户端看看卡不卡”。 |

健康检查不等于完整业务冒烟。下面 PowerShell 先验证进程存活和依赖就绪，任一失败就以非零码退出：

```powershell
$baseUrl = 'http://127.0.0.1:8080'
foreach ($path in '/health', '/ready') {
    try {
        $response = Invoke-WebRequest -UseBasicParsing `
            -Uri ($baseUrl + $path) -TimeoutSec 5
        if ($response.StatusCode -ne 200) {
            throw "HTTP $($response.StatusCode)"
        }
        Write-Host "[OK] $path $($response.Content)"
    }
    catch {
        Write-Error "[FAIL] $path $($_.Exception.Message)"
        exit 1
    }
}
```

业务冒烟可直接使用已经构建的 Qt 真实集成程序。它会创建随机双用户，覆盖登录、WebSocket、重复消息 ACK、READ、历史和文件上传下载，最后注销测试账号：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:CLIENT_BACKEND_URL = 'http://127.0.0.1:8080'
& .\client\build\qt-windows\test_backend_integration.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

前提：正常后端及基础设施已启动，客户端测试已构建。它在当前后端写真实测试数据，不要随手指向重要环境。

## 7. `.env` 到底是什么，为什么 Go 服务不会自动读它

`.env` 是适合本地/部署工具读取的 `键=值` 配置文件，用来放地址、端口、数据库名、账号密码、JWT 密钥等。**它不是操作系统的魔法文件，也不是 Go 标准库会自动加载的配置。**

IMFT 的 Go 配置代码只调用 `os.Getenv`，也就是读当前进程环境变量。直接执行 `go run ./cmd/server` 时，即使旁边躺着 `.env`，服务也不会因此知道里面的值。

本地之所以方便，是脚本补上了这一步：

- `backend/start-local.ps1` 通过 `scripts/_common.ps1` 找到根 `.env` 或 `deploy/.env`，解析所需值，设置当前进程及其子进程的 `$env:*`，再构建并启动 Go 服务。
- Compose 启动容器时，脚本显式传 `--env-file`，Compose 再把需要的环境变量传给对应容器。
- `deploy/.env.stress` 只放独立压测配置；压测脚本会组合正常 env 与 stress env。

真实 env 不能提交，因为它含数据库密码、MinIO 凭据和 JWT 密钥。提交后即使删除，历史记录和副本也可能已经泄露。可以提交的是 `.env.example`：只放安全占位值和说明。首次 `dev-up.ps1` 会生成随机本地密码写入被忽略的 `deploy/.env`，`start-local.ps1` 还会持久化 JWT 密钥，避免后端一重启旧 token 就全部失效。

实践规则：不要把真实密码写进文档、测试断言或命令回显；不要把 `.env` 当普通配置示例发给别人；泄露后应轮换密钥，而不只是从工作区删除文件。

## 8. 启动顺序及原因（含 ROOT runner 的额外构建）

### 8.1 正常聊天

以下命令均在项目根目录执行，除非特别说明。

**第一步：先起 MySQL/MinIO。** 后端启动时会 ping MySQL、执行迁移并确保 MinIO Bucket 存在，所以基础设施不准备好，后端就不该假装启动成功。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\dev-up.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\doctor.ps1
```

**第二步：启动正常 Go 后端。** 在独立 PowerShell 窗口保持运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\backend\start-local.ps1
```

它会读取 env、构建到本地缓存、自动迁移并监听 `127.0.0.1:8080`。看到启动日志后，可执行上一节的 health/ready 冒烟。

**第三步：构建 Qt 客户端。** preset 必须在 `client` 目录使用：

```powershell
Set-Location client
.\tools\fetch_protoc.ps1
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
Set-Location ..
```

Qt preset 固定了本机工具链路径；换机器要按实际 Qt/CMake 路径调整配置，不要把它当成所有电脑都一样的安装命令。

**第四步：启动两个不同用户。** 分两个 PowerShell 窗口执行：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
& .\client\build\qt-windows\bin\secure-im-client.exe --data-dir .\runtime --server-url http://127.0.0.1:8080 --username ALICE
```

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
& .\client\build\qt-windows\bin\secure-im-client.exe --data-dir .\runtime --server-url http://127.0.0.1:8080 --username BOB
```

一个进程对应一个已激活用户；不同用户可以共享 `runtime` base，同一用户目录有锁，不能同时激活。

### 8.2 ROOT 压测为什么还要额外构建 runner

普通 CMake 构建会生成正式客户端、simulator 和测试程序，但真正的负载发生器是 Go 项目 `backend/cmd/stress-runner`。正式客户端 ROOT 页默认寻找：

```text
client/build/qt-windows/bin/secure-im-stress-runner.exe
```

因此，如果要实际点击“开始测试”，还需在项目根目录额外执行：

```powershell
go -C backend build -o ../client/build/qt-windows/bin/secure-im-stress-runner.exe ./cmd/stress-runner
```

`client/start-simulator.ps1` 在不加 `-SkipBuild` 时会顺带完成这项 runner 构建；单纯构建正式 Qt 客户端不会自动完成。

ROOT 使用独立的 `18080` 后端、数据库 `secure_im_stress` 和 Bucket `secure-im-stress-files`，但复用正常环境的 MySQL/MinIO 容器和网络。因此正常基础设施仍须先健康，**正常 8080 后端不是压测后端的替代品**。可以手动准备：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stress-up.ps1
```

正式客户端进入 ROOT 时也会先执行 Setup，再真实登录 ROOT；点击开始后才启动 runner。`stress-up.ps1` 需要构建时，会先在宿主机编译 Linux 后端二进制，再用离线 Dockerfile 制作压测镜像。也就是说，ROOT 链路有两次不同目的的构建：**后端 Linux 镜像用于被测服务，Windows runner 用于制造负载。**

停止时先关客户端、停正常后端终端，再执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stress-down.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\dev-down.ps1
```

`dev-down.ps1` 默认保留数据卷；不要随手加 `-RemoveVolumes`，那会永久删除本地正常和压测数据。

## 9. Docker 文件和 PowerShell 脚本分别干什么

### 9.1 Docker/Compose：描述“服务怎样运行”

| 文件 | 作用与容易误解之处 |
|---|---|
| `deploy/docker-compose.yml` | 正常环境只编排 MySQL 8 和 MinIO：回环端口、健康检查、固定网络、命名持久卷。正常 Go 后端默认由 `start-local.ps1` 在宿主机运行，不在这个 Compose 里。 |
| `deploy/docker-compose.stress.yml` | 只编排独立压测后端，复用外部 `secure-im-net`；设置 `18080`、独立 DB/Bucket、6 CPU、3 GiB、分块预算和压测认证限流配置。正常与压测逻辑隔离，但物理容器资源仍共享。 |
| `backend/Dockerfile` | 常规多阶段镜像：Go 构建阶段下载模块并编译，Alpine 运行阶段用非 root 用户启动。适合需要完整容器构建流程的场景。 |
| `backend/Dockerfile.offline` | 使用宿主机已经编好的 `backend/docker/secure-im-server` 和已缓存的 MinIO 基础镜像组装运行镜像，避免构建时再去拉 Go 构建镜像。它不是另一种后端实现。 |
| `backend/.dockerignore` | 限制 Docker build context，避免把缓存等无关内容送进构建。 |
| `deploy/.env.example`、`.env.stress.example` | 可供版本管理的安全配置样例，供真实本地 env 生成/检查参考。 |

### 9.2 ps1：把人工步骤变成可重复流程

| 脚本 | 负责什么 |
|---|---|
| `scripts/_common.ps1` | 公共函数：找 env、调用 Compose、检查 Docker、等待容器健康、初始化/检查私有 Bucket、读取 dotenv。一般被其他脚本引用，不单独当业务入口。 |
| `scripts/dev-up.ps1` | 首次生成随机本地密码，启动 MySQL/MinIO，等待健康，创建并验证私有 Bucket。 |
| `scripts/doctor.ps1` | 检查 Docker、容器健康、MySQL 查询、MinIO 健康和 Bucket 私有性，用于定位“环境问题还是代码问题”。 |
| `scripts/dev-down.ps1` | 停正常基础设施，默认保留卷；`-RemoveVolumes` 才连数据一起删除。 |
| `backend/start-local.ps1` | 读取正常 env、持久化 JWT 密钥、设置 Go 缓存、构建并启动正常 Windows 后端。 |
| `scripts/stress-up.ps1` | 准备独立压测凭据、数据库和授权，必要时构建 Linux 后端与镜像，启动并等待 ready，最后真实验证 ROOT 登录；已就绪时走快速路径。 |
| `scripts/stress-down.ps1` | 只移除独立压测后端，不动正常基础设施和数据卷。 |
| `scripts/test-all.ps1` | 默认串起 gofmt、Go 单测/竞态/vet、Qt 配置构建/CTest、PowerShell 语法和 Compose 示例检查；`-Integration` 再执行真实集成并清理。 |
| `scripts/generate-proto.ps1` | 用仓内离线工具从 proto 再生成受版本控制的 Go 代码；不生成 `client/build` 中由 CMake 负责的 Qt 代码。 |
| `client/tools/fetch_protoc.ps1` | 把客户端生成所需的 protoc 准备到 `client/.tools`，不是全局安装。 |
| `client/start-simulator.ps1` | 构建/启动兼容 simulator，并在非 SkipBuild 路径额外编译 ROOT runner。 |

企业项目里脚本不是“可有可无的说明文字”，而是把启动顺序、健康门槛和清理边界固化下来。读脚本时重点看**它保证了什么、失败会不会停、清理会不会误删数据**。

## 10. `runtime`、源码边界与 REF 的用途

`runtime` 是运行期数据根目录，不应成为第二份源码目录：

```text
runtime/
├── users/<规范化用户名>/     普通用户的会话、历史、配置、下载和临时数据
├── root-stress/             ROOT 独立会话，不混入 users/ROOT
├── stress-reports/<run-id>/ 每次压测独立报告
└── system-test.lock         正式客户端与 simulator 共用的 ROOT 互斥锁
```

普通用户目录下会看到 `inbox/`、`outbox/`、`downloads/`、`temp/`、`keys/`、`logs/`、`config/` 等用途分隔，以及 `client.lock`。这里的关键不是背文件名，而是理解：

- `--data-dir` 是共享 base，真正身份由后端登录用户名决定；历史和令牌不能跨用户目录串用。
- 下载使用临时/原子写入边界，不能把半个文件当成功结果。
- 报告目录包含 `report.html`、`summary.json`、`metrics.csv`、`config.json`、`conclusion.txt`，每次运行隔离，旧报告不会被新运行覆盖。
- `runtime`、真实 env、Docker 命名卷和 `backups/` 中的数据都不是“构建缓存”。删之前先想清楚里面是否有会话、报告、数据库或密钥。
- 改功能应改 `backend/`、`client/`、`proto/`、`scripts/` 等源码；不要直接改 `runtime` 里的运行结果来冒充代码修复，也不要把测试产生的数据提交成源码。

`backups/REF/` 是重构前提交 `5cb97b7` 的只读源码与配置参照，配套 `backups/REF.sha256.txt` 做逐文件完整性核对。它适合回答“重构前的结构/行为是什么”，不适合当前构建，也不是灾难恢复备份。REF 不包含 `.git`、运行数据、真实 env、缓存、构建产物或工具二进制；后续开发不要修改它。旁边的 ZIP 与 REF 是不同用途的备份资产，也不要混进正常阅读路径。

## 11. 企业级思维专题：从 IMFT 提炼可迁移的方法

### 11.1 分层不是多建几个文件夹，而是控制依赖方向

后端 `cmd/server` 负责装配，业务包拥有自己的 service 和窄 repository，`httpx` 统一把业务错误映射到 HTTP；客户端 UI 不直接解析 JSON，core/app 才负责协议与应用逻辑。好处是改 JSON 字段不必翻遍窗口代码，测试调度器也不必启动 Qt 界面。

判断分层是否有效，可以问：“我换掉传输方式或 UI 时，业务规则要跟着改多少？”不是问“层数够不够多”。

### 11.2 事务保护数据库一致性，但不能顺便回滚外部世界

SQL 事务可以让多张表的修改一起成功或失败，例如消息、会话和已读状态。但 MinIO 删除、WebSocket 推送、写本地文件都不是 MySQL 事务能回滚的。文件最终删除因此拆成两阶段，并保留可重试状态；账号注销也避免在数据库事务里调用 MinIO。

以后看到“事务里调远程 API”，要本能地检查：事务会持锁多久？远程成功后本地提交失败怎么办？反过来又怎么办？

### 11.3 幂等是让“再来一次”有确定结果

重试不可避免，所以写操作必须考虑重复。IMFT 用固定消息 ID 和数据库唯一键识别重复消息，用文件/内容锁及状态协议处理重复分块、完成和删除。幂等不是简单地把异常吞掉，更不是一律返回成功：**同 ID 同内容返回原结果，同 ID 不同内容必须拒绝。**

### 11.4 重试必须有对象、次数、间隔和停止条件

客户端消息重试沿用原 ID，并有 ACK 超时、指数退避和最多 3 次完整写入；认证 401 只刷新重放一次；清理任务保留 deleting 后下一轮再试。网络断开、参数错误、权限不足、业务冲突不是同一种失败，不能套一个无限 retry。

重试还可能放大故障，所以要同时考虑幂等、退避、取消和系统当前负载。

### 11.5 背压是允许系统说“我处理不过来了”

WebSocket 每连接发送队列有界，慢消费者会被断开；客户端下载 writer 使用 1 MiB 有界队列，满时暂停网络读取；后端分块操作在槽位或内存不足时等待，而不是继续分配缓冲。没有背压的“高并发”，经常只是把压力转成内存增长和延迟失控。

### 11.6 资源预算要先定边界，再谈性能数字

IMFT 同时存在数据库连接池、WebSocket 总连接数、每用户连接数、活跃上传数、客户端 3 条上传分块通道和 3 条下载文件通道、后端默认 96 个共享分块槽位与 500 MiB 文件内存预算；压测后端另受 6 CPU/3 GiB 限制。

这些数字管不同资源，不能拿 `500 ÷ 5 = 100` 就断言固定有 100 路上传。槽位还覆盖 Complete 和下载，内存还包括额外开销，实际并发取决于排队工作和预算。好的容量设计要能被观测和压测验证，而不是拍脑袋。

### 11.7 迁移兼容是“让旧世界安全走到新世界”

后端按文件名顺序执行嵌入式迁移，用 `schema_migrations` 防重、MySQL 锁防并发升级；`003` 遇到非空旧加密文件数据会拒绝破坏性升级，而不是误当明文继续跑。客户端也有按身份验证、拒绝非空合并的旧目录迁移。

新增表/字段、回填、切换读写、清理旧结构需要分阶段考虑。协议变更也要照顾两端，不能因为“我这边能编译”就假设旧客户端仍可用。已经应用的迁移不要随手改写历史。

### 11.8 可观测性要帮助定位，而不是只打印“出错了”

- 请求日志和 `X-Request-ID` 帮一次请求串起来。
- `/health` 与 `/ready` 区分“进程活着”和“依赖可用”。
- 可选 `/debug/stats` 暴露 goroutine、堆、分块槽位和内存预算；默认关闭，不应直接暴露公网。
- 文件清理记录选择、成功、跳过、失败；压测报告记录配置、时间序列和结论，采样不可用时写 `unknown`，不能编一个漂亮数字。
- 审计写入是尽力而为，不能拿它替代业务状态或错误监控。

好的观测数据应该能回答“哪个用户/请求、哪个阶段、什么资源、失败是否可重试”。

### 11.9 契约测试保护的是协作边界

`proto/im/v1` 是实时消息契约，Go 与 Qt 生成代码都不能手改；REST JSON 的客户端适配集中在 `ApiJsonCodec`。单元测试能证明各自解析正确，真实集成测试进一步证明“当前客户端和当前后端真的说得通”。

修改字段、错误码、状态语义或 ACK/READ 含义时，要同时考虑生成代码、两端适配、兼容性和集成测试。只做本端单元测试，容易出现“两边都绿了，联调却坏了”。

### 11.10 单实例限制必须写进设计，不能靠运气

当前在线状态与实时推送依赖单进程内存 Hub，WebSocket ticket、部分文件锁、调度器和认证限流也有进程内状态。启动两个后端并共享 MySQL/MinIO，不会自动得到跨实例推送、统一在线状态、统一锁或统一限流，更不等于容灾。

要水平扩展，需要重新设计跨节点消息分发/presence、分布式协调、限流、清理任务归属和连接路由等机制。承认当前边界，比口头上说“以后加 Redis 就行”更专业。

## 12. 给学生的一套实际练习

不要以“我把代码全看完了”为目标，以能独立验证下面成果为目标。

### 练习一：跑起来，并留下证据

1. 按第 8 节启动基础设施和正常后端，运行 `doctor.ps1`。
2. 执行 health/ready PowerShell 冒烟，解释两个端点分别排除了什么故障。
3. 构建客户端，启动 ALICE、BOB，各发一条消息并互传一个小文件。
4. 关闭再打开客户端，观察会话、历史、未读和 outbox 恢复；说明哪些状态在 MySQL，哪些在 `runtime/users`，哪些只在内存。

**完成标准：** 服务没启动时你能先查依赖和就绪状态，而不是第一反应改代码。

### 练习二：做业务冒烟

运行第 6 节的 `test_backend_integration.exe`。然后回答：为什么它比 health/ready 更能证明系统可用，又为什么仍不能证明 1000 人负载可用？

再尝试在故意停掉 MinIO 或后端的情况下重跑 health/ready，观察失败位置，完成后恢复环境。不要在练习中使用 `-RemoveVolumes`。

### 练习三：追踪一条消息的完整生命

1. 发送前在 `MessageDelivery.cpp` 找固定 ID 和先写 outbox 的位置。
2. 沿 `WebSocketClient`、`.proto`、后端 `chat/http.go`、`service.go`、`repository.go` 找到 MySQL 持久化和 ACK。
3. 在接收端找实时 MESSAGE；关掉接收端再发送，重开后沿 history cursor 找补拉路径。
4. 打开会话触发 READ，说明 ACK 与 READ 的时间、存储和通知差异。
5. 断网重试同一消息，解释为什么不会正常地产生重复副本；ACK 丢失时历史如何补证。

**完成标准：** 能画出“本地 outbox → WS → MySQL → ACK/在线推送 → 历史补拉 → READ”的图，并标出每个失败恢复点。

### 练习四：追踪一个文件及它的共享内容

1. 从 `TransferUpload.cpp` 跟到 init、分块 PUT、状态查询和 complete，再到后端 `filetransfer`。
2. 上传一个大于一个分块的小测试文件，暂停后继续，找出 `uploaded_chunks` 怎样避免重传已确认块。
3. 用另一逻辑发送复用相同内容，观察 `instant=true`；解释为什么此时不能重复 complete。
4. 下载并暂停后继续，确认当前实现是重新下载而不是 Range 续传。
5. 删除其中一个逻辑引用，再删除最后引用，结合 `deletion.go` 说明 MySQL 元数据和 MinIO 对象分别在何时变化。

**完成标准：** 能讲清“逻辑文件、共享内容、分块对象”三层，不再把 `file_id` 当作物理文件唯一副本。

### 练习五：做一次小而完整的改动

推荐练习：给 `/health` 的 JSON `data` 增加一个 `service: "imft"` 字段。这个改动足够小，又能走完“实现—契约—测试—冒烟”闭环。

1. 先读 `backend/cmd/server/health.go` 和对应路由测试，确认新增字段是向后兼容的加法，不顺便改状态码或其他字段。
2. 做最小实现，并补/更新测试，明确断言新字段，同时保留原有 `status` 承诺。
3. 在 `backend` 执行：

```powershell
gofmt -w .\cmd\server\health.go
# 对你实际改过的测试文件也执行 gofmt
go test ./cmd/server
go test ./...
```

4. 重启正常后端，再跑 health/ready 与业务冒烟。
5. 自查是否误改了生成物、runtime、真实 env，是否把一个简单需求顺手扩成大重构。

**完成标准：** 不只是“本机看着能跑”，而是有针对性测试、完整回归和真实冒烟证据。

### 练习六：把并发与资源当成功能来测

阅读一个调度器/内存预算测试和一个下载背压测试，再选一个小规模 ROOT 配置运行。对照报告中的槽位、内存、错误与延迟，解释为什么提高 worker 数不一定提高吞吐，以及“进程没崩”为什么不能单独作为压测通过标准。

## 13. 自测问题：答不上来就回到对应主线

1. 为什么模块化单体也可以是企业级设计，微服务为什么不能自动解决一致性问题？
2. MySQL、MinIO、内存 Hub 各自保存什么？后端重启后哪些状态会消失，哪些仍能恢复？
3. 为什么 UI 不直接解析后端 JSON？proto 生成文件为什么不能手改？
4. `/health` 返回 200 但 `/ready` 失败，说明什么？为什么业务冒烟还要再跑？
5. Access Token、Refresh Token、WebSocket ticket 为什么不是三个重复概念？
6. 多个请求同时 401 时，为什么只能共享一次刷新？重放为什么只有一次？
7. 为什么消息必须先持久化 outbox 再发送？固定 `message_id` 在服务端怎样防止重复？
8. ACK 丢失是否必然丢消息？ACK 已收到为什么仍可能显示未读？
9. 离线消息为什么能靠 history 恢复，而不是依赖 Hub 永远排队？
10. 秒传为什么需要大小和整文件哈希？未完成的相同内容为什么不能直接秒传？
11. 删除一个文件引用后，为什么 MinIO 对象可能仍需保留？最终删除为什么要分两阶段？
12. 为什么不能在 MySQL 事务里假设 MinIO 删除也能一起回滚？
13. 单元测试、真实集成、冒烟和压测各自不能替代谁？
14. Go 服务旁边有 `.env` 却读不到，原因是什么？脚本和 Compose 分别怎样补上配置？
15. 为什么正常聊天只需 8080，而 ROOT 还要 18080 后端和额外 runner？
16. `backend/Dockerfile` 与 `Dockerfile.offline` 的输入和适用场景有什么不同？
17. 为什么 `runtime`、Docker 数据卷和真实 env 不能当缓存随手清理？
18. REF 能用来做什么，为什么不能拿它覆盖当前源码或当灾难恢复？
19. 96 个分块槽位、500 MiB 预算、400 个 runner 文件 worker 为什么不能直接画等号？
20. 再启动一个后端实例后，在线推送、限流、文件锁会自动跨实例一致吗？还缺哪些设计？

如果你能不看代码回答大部分问题，再沿路径指出证据，你就已经从“会写功能的 Demo 作者”开始向“能接手真实系统的工程师”转变了。
