# Secure IM Qt 客户端

Qt 6 Widgets / C++17 桌面客户端。正式程序 `secure-im-client` 采用**一个进程一个用户**模型，需要多用户时多开进程并为每个用户指定独立数据目录。`secure-im-simulator` 是单独的 ROOT 本地系统压测工具，不是多用户正式客户端。

## 功能与结构

- 正式客户端启动后显示注册/登录页；已有有效本地会话时恢复聊天页。用户名输入自动转大写，密码为 6～128 个 UTF-8 字节。注册和登录分别受后端限流，触发时返回 HTTP 429、`rate_limited` 和 `Retry-After`。
- 访问令牌到期前自动调用 refresh，保存轮换后的访问/刷新令牌；刷新失败后回到登录状态，不继续复用已轮换或已撤销的 Refresh Token。退出会使旧 refresh 及等待请求失效，迟到回调不能覆盖后续登录；如需轮换，退出请求会撤销轮换后的当前 Refresh Token。
- 已登录页提供“退出”和“注销”。“退出”经确认后调用 `POST /api/v1/auth/logout` 撤销当前 Refresh Token，再清除本地会话并关闭窗口；不删除服务器账号，也不撤销其他登录会话。“注销”明确警示永久删除账号及关联消息/文件，仅在 `DELETE /api/v1/users/me` 返回 204 后清理本地会话。
- 联系人搜索、用户列表与 presence 周期刷新、单聊历史增量拉取、WebSocket 实时收发。联系人及会话顶部显示在线状态；自己的消息区分未读/已读并显示已读时间，ACK 不代表已读。
- 仅当前可见聊天会话发送最新 incoming cursor 的已读回执。未查看来信按联系人显示红点，历史消息和配置不跨用户目录共享。
- 聊天与文件传输分视图；任务卡片显示方向、文件名、对方、进度、大小、状态、路径及错误，支持暂停、继续、取消、失败/取消后重试和打开目录。
- 上传固定使用 **3 条分块通道**，下载固定使用 **3 条文件通道**，二者独立。上传按分块动态调度：就绪新文件按 FIFO 优先开始，其余通道分配给剩余量最大的文件；单文件不同 index 可同时在途，全局 PUT 不超过 3。
- 正常客户端请求/回退 **5 MiB** 分块，并接受服务端返回的 `chunk_size`；逐块 SHA-256，通过 `uploaded_chunks` 跳过服务端已有分块。全系统按 **size + 整文件 SHA-256** 复用已完成内容，`instant=true` 时跳过上传和 complete。
- 文件哈希和分块读取在进程级默认 **4 线程**的公共 `QThreadPool` 执行。下载通过 `readyRead` 交给该文件的顺序 writer 线程，以 `QSaveFile` 原子提交并校验大小；writer 使用 **1 MiB** 有界字节队列，达到上限后通过 reply 读缓冲实施网络背压，消费后自动恢复，不并行写同一文件，也不在内存中累积整个文件。
- 上传和下载暂停会中止当前网络请求；上传暂停后继续会查询已确认分块并续传。下载暂停后继续会重新获取完整文件，因为当前接口未实现 Range。
- 下载文件名经净化并去除路径部分，阻止路径穿越及 Windows 保留设备名，重名文件自动改名；Windows 上的并发下载路径预留不区分大小写。

```text
src/core/ApiClient.*          REST 路径、请求和响应 JSON 的适配层
src/core/WebSocketClient.*   异步 QWebSocket
src/core/WireCodec.*         二进制帧接口
src/core/ProtobufWireCodec.* Protobuf v2 Envelope 与 typed payload 编解码
src/core/MessagePayloadCodec.* 文本/文件通知 MessagePayload 编解码
src/core/TransferManager.*   固定独立通道、分块动态调度及传输状态机
src/core/LocalStore.*        每用户目录、会话、配置和历史
src/core/Workers.*           哈希、文件 I/O 与流式原子写入
src/core/MessageSyncState.h  消息去重及数字 cursor 状态
src/ui/SessionContext.*    每进程独立对象、目录锁及幂等 shutdown
src/ui/SessionPage.*       登录页与聊天页切换
src/ui/MainWindow.*        聊天/传输业务界面
src/ui/SimulatorWindow.*   ROOT 登录及单一系统压测窗口
src/ui/StressRunController.* stress-up、runner 生命周期及 NDJSON 解析
src/ui/SystemTestPage.*    压测配置、实时指标、结论与历史报告
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
  --data-dir D:/im-data/alice --server-url http://127.0.0.1:8080 --username alice
```

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe `
  --data-dir D:/im-data/bob --server-url http://127.0.0.1:8080 --username bob
```

`--data-dir` 必填且不能是文件系统根目录；两个用户不得共用，客户端通过 `config/client.lock` 拒绝并发使用同一目录。`--username` 只预填登录用户名，可省略。每个目录独立创建 `inbox/`、`outbox/`、`downloads/`、`temp/`、`keys/`、`logs/`、`config/`。令牌不会写入日志，会话文件尽量设置为仅所有者可读写。

## ROOT 本地系统压测

```powershell
client/start-simulator.ps1
```

压测工具固定使用 **ROOT / 123456**，该凭据只用于 `http://127.0.0.1:18080` 的本地隔离压测入口，不是正式客户端的默认账号。启动测试时先执行 `scripts/stress-up.ps1`，再启动 `client/build/qt-windows/bin/secure-im-stress-runner.exe`，仅传入 `--target`、`--server`、`--env-file`、`--report-root`。

固定负载为 80/18/2 空闲/聊天/文件、500 MiB 文件预算、**5 MiB** 分块、96 个文件 worker、100 MiB 单文件和 **5 分钟**稳定期。独立后端限制 **6 CPU / 3 GiB**，使用独立数据库 `secure_im_stress` 和 Bucket `secure-im-stress-files`，同时共享正常环境的 MySQL/MinIO 容器。报告写入 `runtime/stress-reports` 并读取各目录 `summary.json`。

`SECURE_IM_STRESS_RUNNER`、`SECURE_IM_STRESS_REPORT_ROOT`、`SECURE_IM_STRESS_SERVER` 与 `SECURE_IM_STRESS_SKIP_SETUP=1` 仅作为开发/测试覆盖。runner 自身保留 `--chunk-size`、`--stable-duration` 等命令行覆盖。

## 后端对齐点

所有 REST 响应字段适配集中在 `ApiClient.cpp`，UI 不直接读取后端 JSON 字段。

| 接口 | 当前请求/响应约定 |
|---|---|
| `POST /api/v1/auth/register`、`login` | JSON `username/password`，注册附带 `display_name`；读取 `data.user` 和 `data.tokens.access_token/refresh_token` |
| `POST /api/v1/auth/refresh` | JSON `refresh_token`；响应同登录，旧 Refresh Token 被轮换 |
| `POST /api/v1/auth/logout` | Bearer + JSON `refresh_token`；204 幂等撤销当前用户的该 Refresh Token |
| `GET /api/v1/users`、`presence` | 用户列表及 `data.online_user_ids` |
| `POST /api/v1/ws-ticket` | 读取 `ticket`，随后连接 `ws(s)://host/ws?ticket=...` |
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

默认 CTest 覆盖 API 响应适配、logout/refresh 竞态、Protobuf v2 编解码、消息同步、LocalStore、上传动态调度与续传、取消删除后立即重试、秒传、独立下载并发、Windows 路径预留、慢 writer 背压和流式写入、ROOT 压测窗口、runner NDJSON 解析及报告历史。预览测试在 `QT_QPA_PLATFORM=offscreen` 下生成 PNG。

`test_backend_integration` 不注册到默认 CTest，可对运行中的后端执行随机双用户注册/登录、WebSocket 消息、ACK、READ、history 和文件上传/下载，最后注销测试账号：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:CLIENT_BACKEND_URL = "http://127.0.0.1:8080"
client/build/qt-windows/test_backend_integration.exe
```

当前范围不包含 QML、群聊和 E2EE。
