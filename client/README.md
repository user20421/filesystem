# 即时通讯与文件传输系统 Qt 客户端

Qt 6 Widgets / C++17 桌面客户端。正式程序 `secure-im-client` 采用**一个进程一个已激活用户**模型；`--data-dir` 指定共享 base，后端返回的登录用户名决定 base 下的独立用户目录。需要多用户时可多开进程，不同用户可以共享 base，同一用户目录仍禁止并发使用。正式客户端的同一登录页也提供 ROOT 本地系统压测入口；`secure-im-simulator` 保留为兼容的独立压测工具。

## 功能与结构

- 正式客户端启动后显示注册/登录页；已有有效本地会话时恢复聊天页。用户名输入自动转大写，密码为 6～128 个 UTF-8 字节。注册和登录分别受后端限流，触发时返回 HTTP 429、`rate_limited` 和 `Retry-After`。
- 访问令牌到期前自动调用 refresh，保存轮换后的访问/刷新令牌；刷新失败后回到登录状态，不继续复用已轮换或已撤销的 Refresh Token。退出会使旧 refresh 及等待请求失效，迟到回调不能覆盖后续登录；如需轮换，退出请求会撤销轮换后的当前 Refresh Token。
- 已登录页提供“退出”和“注销”。“退出”经确认后调用 `POST /api/v1/auth/logout` 撤销当前 Refresh Token，再清除本地会话并关闭窗口；不删除服务器账号，也不撤销其他登录会话。“注销”明确警示永久删除账号及关联消息/文件，仅在 `DELETE /api/v1/users/me` 返回 204 后清理本地会话。
- 联系人搜索、用户列表与 presence 周期刷新、单聊历史增量拉取、WebSocket 实时收发。联系人及会话顶部显示在线状态；自己的消息区分未读/已读并显示已读时间，ACK 不代表已读。
- 文本与文件通知共用持久化 outbox：先写盘再发送，固定 `message_id`、payload 和时间戳，最多接受 3 次完整帧写入；约 12 秒 ACK 超时后指数退避，断线不消耗次数。ACK/ERROR 按 ID 更新，迟到 ACK 或 history 可恢复失败项；启动恢复 pending/failed 并对账相关联系人，shutdown 保留 pending。界面区分等待发送、等待 ACK、失败、已确认未读和已读。
- 登录或恢复本地会话后先请求一次批量未读摘要，再加载联系人；摘要先于联系人返回时也会保留计数并显示红点，未查看来信按联系人显示红点。仅当前可见聊天会话发送最新 incoming cursor 的已读回执，进入会话加载本地与离线历史并清除红点；历史消息和配置不跨用户目录共享。
- 正式 `SingleUserWindow` 的普通登录页和聊天主界面统一固定为 **560×390**，不能拖动调整窗口；联系人与聊天区域的 `QSplitter` 手柄宽度为 0 且禁用。ROOT 验证成功后压测页调整为可缩放的 1280×820 初始尺寸，返回普通登录页时恢复固定尺寸。
- 聊天与文件传输分视图；任务卡片显示方向、文件名、对方、进度、大小、状态、路径及错误，支持暂停、继续、取消、失败/取消后重试和打开目录。
- 上传固定使用 **3 条分块通道**，下载固定使用 **3 条文件通道**，二者独立。上传按分块动态调度：就绪新文件按 FIFO 优先开始，其余通道分配给剩余量最大的文件；单文件不同 index 可同时在途，全局 PUT 不超过 3。
- 正常客户端请求/回退 **5 MiB** 分块，并接受服务端返回的 `chunk_size`；逐块 SHA-256，通过 `uploaded_chunks` 跳过服务端已有分块。全系统按 **size + 整文件 SHA-256** 复用已完成内容，`instant=true` 时跳过上传和 complete。
- 文件哈希和分块读取在进程级默认 **4 线程**的公共 `QThreadPool` 执行。下载通过 `readyRead` 交给该文件的顺序 writer 线程，以 `QSaveFile` 原子提交并校验大小；writer 使用 **1 MiB** 有界字节队列，达到上限后通过 reply 读缓冲实施网络背压，消费后自动恢复，不并行写同一文件，也不在内存中累积整个文件。
- 上传和下载暂停会中止当前网络请求；上传暂停后继续会查询已确认分块并续传。下载暂停后继续会重新获取完整文件，因为当前接口未实现 Range。
- 下载文件名经净化并去除路径部分，阻止路径穿越及 Windows 保留设备名，重名文件自动改名；Windows 上的并发下载路径预留不区分大小写。

```text
src/core/ApiClient*.cpp      transport、refresh/logout、endpoint 与 history 实现拆分
src/core/ApiClient.h         稳定的公共 API、认证 epoch 与流式请求句柄
src/core/ApiJsonCodec.*      REST 响应 JSON 与 legacy 字段分支的纯适配逻辑
src/core/WebSocketClient.*   异步 QWebSocket
src/core/WireCodec.*         二进制帧接口
src/core/ProtobufWireCodec.* Protobuf v2 Envelope 与 typed payload 编解码
src/core/MessagePayloadCodec.* 文本/文件通知 MessagePayload 编解码
src/core/TransferManager.*   任务与 Manager 公共接口及生命周期
src/core/Transfer*.cpp       3+3 通道调度、上传、下载、取消重试实现
src/core/LocalStore.*        每用户目录、会话、配置和历史
src/core/LocalStoreMessageCodec.* 本地消息 JSON 与合并规则内部组件
src/core/Workers.*           哈希、文件 I/O 与流式原子写入
src/core/MessageDelivery.*   持久化 outbox、ACK 超时、重试及恢复
src/core/MessageSyncState.h  消息去重及数字 cursor 状态
src/app/RootStressSession.* 独立 ROOT 认证上下文与压测配置
src/app/SessionContext.*    base、按用户激活/隔离、目录锁、旧数据迁移及幂等 shutdown
src/app/StressRunController.* stress-up 与 runner 两阶段进程生命周期
src/app/StressNdjsonCodec.* runner NDJSON 单行纯解码
src/app/StressReportRepository.* 历史 summary.json 读取、schema 适配与排序
src/app/StressAssessment.*  最终指标与瓶颈建议纯逻辑
src/ui/SessionPage.*       登录页、聊天页与集成 ROOT 压测页切换及显式关闭顺序
src/ui/SingleUserWindow.*  正式客户端固定尺寸顶层窗口
src/ui/MainWindow*.cpp     UI 构建、联系人未读、聊天历史 READ 与传输视图
src/ui/SimulatorWindow.*   独立 ROOT 压测窗口
src/ui/SystemTestPage.*    压测配置、实时指标、结论与历史报告展示
src/apps/                  正式单用户入口与 ROOT 压测工具入口
tests/                     QtTest/CTest 核心及集成测试
```

## 构建依赖

- 支持 C++17 的编译器及 CMake 3.16+
- Qt 6 开发包：`Widgets`、`Network`、`WebSockets`、`Test`、`Protobuf`、`ProtobufTools`
- 宿主机 `protoc`，仅用于调用 Qt 的 `qtprotobufgen`；客户端链接 Qt6 Protobuf，不链接 Google Protobuf 运行库

工程从 `../proto/im/v1/envelope.proto` 和 `messages.proto` 生成 Qt Protobuf 源码。实时 Protobuf v2 是必需功能：缺少 Qt Protobuf 或生成工具时 CMake 会明确失败。

本机 Qt 6.11.2 Preset 已固定编译器和 Qt 前缀。Qt 安装不含独立 protoc，以下脚本将官方 protoc 29.3 下载到 `client/.tools/`，不进行全局安装：

```powershell
Set-Location client
./tools/fetch_protoc.ps1
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
& "D:/software/Qt/Tools/CMake_64/bin/ctest.exe" --preset qt-windows
```

其他环境可在 PATH 提供 protoc，或配置时设置 `CLIENT_PROTOC_EXECUTABLE` 和 `CMAKE_PREFIX_PATH`。

## 运行正式客户端

在两个终端分别运行，例如：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe `
  --data-dir D:/im-data --server-url http://127.0.0.1:8080 --username alice
```

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe `
  --data-dir D:/im-data --server-url http://127.0.0.1:8080 --username bob
```

`--data-dir` 必填且不能是文件系统根目录，它只是 base。启动时只准备 base；普通用户登录或恢复成功后，才按后端返回并转为大写的用户名激活 `<base>/users/<规范化用户名>/`，例如 ALICE 直接使用 `<base>/users/ALICE/`，不再有 `u-` 前缀或启动别名嵌套。Windows 保留设备名、尾点/尾空格、控制字符和路径分隔符等会被安全处理，净化后的不同用户名通过稳定后缀避免碰撞和路径穿越。会话恢复只扫描 `users/` 的直接子目录。每个用户目录独立创建 `inbox/`、`outbox/`、`downloads/`、`temp/`、`keys/`、`logs/`、`config/`，下载写入当前用户的 `downloads/`，上传仍可选择任意本机可读路径。

用户锁位于 `<base>/users/<规范化用户名>/client.lock`，只拒绝同一用户目录的并发激活，不阻止 ALICE 与 BOB 共享 base。`--username` 既预填登录名，也定向选择匹配的已保存会话：没有该参数时只有唯一有效会话才自动恢复，存在多个会话时绝不按目录顺序任意选择。令牌不会写入日志，会话文件尽量设置为仅所有者可读写。

旧版本直接位于 base 的 `config/`、`downloads/` 可以迁移到用户目录：旧 `config/session.json` 含有效身份时按该用户名自动迁移；无有效身份时首次登录会明确询问，选择不迁移会保留旧数据。迁移使用 base 内的临时目录和目录 rename，失败会尽量回滚；目标 `config/` 或 `downloads/` 非空时禁止合并。旧 `<base>/users/u-用户名` 及历史 `<base>/alice/users/u-用户名`、`<base>/bob/users/u-用户名` 也会迁移到新同级目录，但仅当保存会话的用户 ID 与规范化用户名均匹配时才整体 rename；目标非空、多个匹配来源或旧锁被占用都会停止，rename 失败不删除来源。迁移后仅对空历史容器调用 `rmdir`，任何非空别名或 `users/` 容器均原样保留。

## ROOT 本地系统压测

按“运行正式客户端”启动 `secure-im-client`，点击登录页底部 **“系统测试”** 按钮，或在同一登录页输入 **ROOT / 123456**（用户名大小写不敏感）。进入顺序固定为取得 `<项目>/runtime/system-test.lock` → 只执行 `stress-up.ps1` Setup（不启动 runner）→ 向独立压测后端 **`http://127.0.0.1:18080`** 发出真实 `POST /api/v1/auth/login`；只有验证成功才在当前 `SingleUserWindow` 显示 `SystemTestPage`。Setup 的标准输出/错误会保留在失败原因中，密码错误、服务未启动或准备失败都会销毁本次失败对象、释放锁并留在登录页，可直接重试。正式客户端与 simulator 共用该锁，第二客户端会看到明确占用提示且不得 Setup 或登录。普通用户名仍请求 `--server-url` 指定的正式后端（默认 `http://127.0.0.1:8080`）并进入聊天。

ROOT 使用当前 base 下独立的 `<base>/root-stress/` 会话目录，不创建 `<base>/users/ROOT`，不读取或覆盖任何普通用户的 `config/session.json` 及其他既有本地文件，也不复用普通用户的 `ApiClient`。压测页左上角“返回普通登录”会先停止并等待回收当前压测 runner，再销毁压测页、释放 `runtime/system-test.lock` 并恢复固定窗口尺寸，随后可正常登录普通账号；直接关闭窗口也按相同顺序清理。兼容入口 `client/start-simulator.ps1` 仍启动独立 `secure-im-simulator`。

可用 `SECURE_IM_STRESS_SERVER` 覆盖压测后端地址（主要用于开发/测试）。`StressRunController::prepare()` 只执行 `scripts/stress-up.ps1`；点击开始测试后，`start()` 仍按 Setup → runner 顺序执行，runner 仅传入 `--target`、`--server`、`--env-file`、`--report-root`。`stress-up.ps1` 会先检查 `/ready` 并真实登录 ROOT 验证令牌，已就绪时直接返回，不重复构建。

固定负载为 80/18/2 空闲/聊天/文件、500 MiB 文件预算、**5 MiB** 分块、400 个文件 worker、100 MiB 单文件和 **5 分钟**稳定期。独立后端限制 **6 CPU / 3 GiB**，使用独立数据库 `secure_im_stress` 和 Bucket `secure-im-stress-files`，同时共享正常环境的 MySQL/MinIO 容器。系统测试页使用两行、8 张卡片的紧凑网格；文件预算卡片同时展示“分块通道”，从 runner NDJSON 的 `file_chunk_slots_used` / `file_chunk_slots_limit` 实时显示后端实际占用/上限，未运行显示 `—`，旧 runner 或未知采样显示“未知”。报告写入 `runtime/stress-reports` 并读取各目录 `summary.json`，`metrics.csv` 同样追加分块通道两列，未知值写为 `unknown`。

`SECURE_IM_STRESS_RUNNER`、`SECURE_IM_STRESS_REPORT_ROOT`、`SECURE_IM_STRESS_SERVER`、`SECURE_IM_STRESS_SETUP_SCRIPT`、`SECURE_IM_STRESS_POWERSHELL` 与 `SECURE_IM_STRESS_SKIP_SETUP=1` 仅作为开发/测试覆盖。runner 自身保留 `--chunk-size`、`--stable-duration` 等命令行覆盖。

## 后端对齐点

REST 响应字段适配集中在 `ApiJsonCodec.*`，各 endpoint 和历史同步分别位于 `ApiClientEndpoints.cpp`、`ApiClientHistory.cpp`；UI 不直接读取后端 JSON 字段。

| 接口 | 当前请求/响应约定 |
|---|---|
| `POST /api/v1/auth/register`、`login` | JSON `username/password`，注册附带 `display_name`；读取 `data.user` 和 `data.tokens.access_token/refresh_token` |
| `POST /api/v1/auth/refresh` | JSON `refresh_token`；响应同登录，旧 Refresh Token 被轮换 |
| `POST /api/v1/auth/logout` | Bearer + JSON `refresh_token`；204 幂等撤销当前用户的该 Refresh Token |
| `GET /api/v1/users`、`presence` | 用户列表及 `data.online_user_ids` |
| `POST /api/v1/ws-ticket` | 读取 `ticket`，随后连接 `ws(s)://host/ws?ticket=...` |
| `GET /api/v1/messages/unread` | 一次读取 `data.unread[]` 中各联系人的 `peer_id/unread_count/last_message_at`，用于启动恢复红点 |
| `GET /api/v1/messages?peer_id=&after_id=` | `cursor` 用于下次 `after_id`；`envelope` 为 Base64 Protobuf v2 Envelope |
| `POST /api/v1/messages/read` | JSON `peer_id/cursor`；服务端通过 WS READ 通知双方连接 |
| `POST /api/v1/files/init` | `recipient_id/filename/size/sha256/chunk_size`；读取 `data.file.id/chunk_size/instant` 等 |
| `GET /api/v1/files/{id}` | 读取 `data.file` 及同级 `uploaded_chunks` |
| `PUT /api/v1/files/{id}/chunks/{index}` | 原始明文字节及 `X-Chunk-SHA256`，索引从 0 开始 |
| `POST /api/v1/files/{id}/complete` | 空 JSON 对象；仅非 instant 内容需要 |
| `GET /api/v1/files/{id}/download` | `readyRead` 流式读取，顺序 writer 校验大小后原子提交 |
| `DELETE /api/v1/files/{id}` | 取消上传或删除当前逻辑引用 |
| `DELETE /api/v1/users/me` | Bearer 认证；仅 204 视为永久注销成功 |

WebSocket 外层使用 `im.v1.Envelope`，`version=2`。MESSAGE payload 是 `MessagePayload`（文本或文件通知），ACK payload 是 `AckPayload`，ERROR payload 是 `ErrorPayload`；不存在旧 UTF-8 JSON payload 或十进制 cursor ACK。历史接口的 Base64 仅封装完整 Envelope 字节，不改变 payload 的 Protobuf 格式。

文件分块在 MinIO 中以明文保存，不使用 DEK/KEK 或应用层密文。文件内容按 size + SHA-256 全局复用，删除最后一个逻辑引用后才清理对象。

## 测试

默认 CTest 覆盖 API 响应适配、批量未读摘要解析、启动恢复红点及阅读清除、固定窗口与禁用分割手柄、logout/refresh 竞态、不同用户目录隔离与用户锁、特殊用户名净化、无前缀目录及历史目录按身份迁移、目标非空/多来源/旧锁拒绝、唯一/定向会话恢复、切换用户后的迟到刷新回调隔离、ROOT 不嵌套用户目录、Protobuf v2 编解码、消息同步、LocalStore、离线 outbox 持久化、同 ID 重试/最大次数、ACK/ERROR/迟到 ACK、history 对账、文件通知、重启、写盘失败、pending 裁剪与 shutdown、上传动态调度与续传、取消删除后立即重试、秒传、独立下载并发、Windows 路径预留、慢 writer 背压和流式写入、ROOT 压测窗口、正式客户端 ROOT 真实登录路由/失败拦截/会话隔离/返回普通登录、Setup-only 与失败输出、Setup 先于登录及失败重试、第二客户端互斥、runner NDJSON 解析及报告历史。UI 测试在 `QT_QPA_PLATFORM=offscreen` 下运行。

`test_backend_integration` 不注册到默认 CTest，可对运行中的后端执行随机双用户注册/登录、WebSocket 消息、重复帧返回相同 ACK cursor、READ、history 和文件上传/下载，最后注销测试账号：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:CLIENT_BACKEND_URL = "http://127.0.0.1:8080"
client/build/qt-windows/test_backend_integration.exe
```

当前范围不包含 QML、群聊和 E2EE。
