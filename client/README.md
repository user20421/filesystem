# Secure IM Qt 客户端

Qt 6 Widgets / C++17 桌面客户端。`secure-im-client` 保留单用户入口；`secure-im-simulator` 是现代浅色主题的单窗口多用户工作台，每个可切换标签承载一个完全独立的用户会话，不再启动 `QProcess` 子进程。

## 功能与结构

- 顶部标签顺序固定为“系统测试”“欢迎”和各用户标签；前两个固定标签不显示关闭按钮，用户标签支持新建、关闭和拖动。系统测试是内嵌预留页面，供后续大量用户消息与文件压力测试使用，不再启动独立窗口。欢迎页仅提供“新增用户”，并支持右上角 `+`、`Ctrl+T`、`Ctrl+W`、`Ctrl+Tab`；数据目录与服务器地址均只读固定展示。
- 每个标签独立持有 `SessionContext`（`LocalStore`、`QLockFile`、`ApiClient`、WireCodec、`AuthSession`），隐藏标签继续保持连接、同步和传输；后台新消息按联系人累计未读数。
- 已登录页右上角连接状态旁提供“退出”和“注销”。“退出”经确认后仅清除该标签的 `config/session.json`，复用幂等 `shutdown` 和 `closePage` 停止连接/传输并关闭标签，随后回到欢迎页，不删除服务器账号；其他标签和历史文件不受影响。“注销”默认选择“否”，明确警示永久删除服务器账号及关联消息/文件且不可恢复；仅 `DELETE /api/v1/users/me` 返回 204 后才清会话并关闭标签，失败则显示错误、保留页面和会话以便重试。单用户程序在两者成功后均关闭窗口。
- 注册、登录（用户名输入自动转大写，密码 6~128 个 UTF-8 字节）、联系人搜索、用户列表与 presence 同周期刷新、单聊历史轮询、WebSocket 实时收发。`QTIT_` 前缀的自动化集成账号不会显示在联系人窗口。
- 未查看的来信在对应联系人姓名旁显示红色圆点；进入该联系人的可见聊天视图后仅清除此联系人的未读，标签总未读为各联系人之和。切换联系人、搜索或定时刷新用户列表不会丢失其他联系人的未读。
- 联系人和当前会话顶部显示在线/离线；界面不直接显示 UUID，用户 ID 保留在内部数据、搜索和 tooltip 中。presence 请求失败保留上次状态。自己的消息区分未读/已读并显示已读时间，ACK 不代表已读。
- 仅当前活动标签的可见聊天会话发送最新 incoming cursor 的已读回执；切换回会话会补发。服务端 READ 只合并本地 `read_at`，不会生成气泡、未读或下载任务。
- 聊天与文件传输通过导航切换；传输卡片显示独立进度条、状态和错误，支持暂停、继续、取消和失败/取消后重试；“打开目录”按钮以及单击文件名/本地路径均打开文件父目录。
- 集中式 `Theme` 提供浅色 QSS、圆角卡片、主色、字体、间距及 hover/pressed/disabled 状态，无 QML 或第三方主题。
- 文件级并发队列可设为 **3~5**，默认 3；上传默认 4 MiB 分块（也接受服务端返回的 `chunk_size`），逐块 SHA-256，并通过文件状态中的 `uploaded_chunks` 跳过服务端已有分块。
- HTTP 与 WebSocket 均为异步。文件哈希和分块读取在 `QThreadPool` 执行；下载通过 `readyRead` 分块送入顺序 writer 线程，以 `QSaveFile` 原子提交并校验大小，不在内存中累积整个文件。失败、暂停和取消会清理未提交数据；小量 JSON 配置读写仅发生在 UI 的本地元数据路径。
- `MessageSyncState` 统一处理历史 cursor 与 optimistic 去重：重复消息仍先推进 cursor，实时消息不会用 UUID 覆盖数字 cursor，旧 history 不会使 cursor 倒退。
- `LocalStore` 持久化并合并重复消息的 cursor/readAt；重复 history 不会清除已读状态，READ 与 presence 也不参与聊天 cursor 同步。
- 每个 `--data-dir` 下独立创建 `inbox/`、`outbox/`、`downloads/`、`temp/`、`keys/`、`logs/`、`config/`。历史消息和配置不跨目录共享。令牌不会写入日志，会话文件尽量设置为仅所有者可读写；生产环境可在 `LocalStore` 替换为系统钥匙串。
- 下载文件名经净化并去除路径部分，阻止路径穿越及 Windows 保留设备名，重名文件自动改名。

```text
src/core/ApiClient.*          REST 路径、请求和响应 JSON 的唯一适配层
src/core/WebSocketClient.*   异步 QWebSocket
src/core/WireCodec.*         可替换的二进制帧接口
src/core/ProtobufWireCodec.* qtprotobufgen 生成类的 Envelope 编解码
src/core/TransferManager.*   文件级队列、分块续传及传输状态机
src/core/LocalStore.*        每用户目录、会话、配置和历史
src/core/Workers.*           工作线程哈希、文件 I/O 与流式原子写入
src/core/MessageSyncState.h  可测试的消息去重及数字 cursor 状态
src/ui/SessionContext.*    每标签独立对象、目录锁及幂等 shutdown
src/ui/SessionPage.*       可嵌入登录页与聊天页，切换登录状态
src/ui/MainWindow.*        可嵌入 QWidget 聊天/传输业务界面
src/ui/SimulatorWindow.*   QMainWindow + QTabWidget 多用户工作台
src/ui/SystemTestPage.*    内嵌的系统测试预留页面
src/ui/Theme.*             集中的浅色 Qt Widgets 主题
src/apps/                  单用户入口与多用户 simulator
tests/                     QtTest/CTest 核心及多标签生命周期测试

`client_ui` 静态库复用上述全部 UI，由单用户程序、simulator 和 UI 测试共同链接。
```

## 构建依赖

- 支持 C++17 的编译器及 CMake 3.16+
- Qt 6 开发包：`Widgets`、`Network`、`WebSockets`、`Test`、`Protobuf`、`ProtobufTools`（qmake 不是构建依赖）
- 宿主机 `protoc`，**仅用于调用 Qt 的 `qtprotobufgen`**；客户端链接 Qt6 Protobuf，不链接 Google Protobuf 运行库

工程从 `../proto/im/v1/envelope.proto` 通过 `qt_add_protobuf` 生成 Qt Protobuf 源码，不手写二进制协议。实时 Protobuf 是必需功能：缺少 Qt Protobuf 或生成工具时 CMake 会明确失败，不会静默禁用 WebSocket 编解码。

本机 Qt 6.11.2 Preset 已固定用户提供的编译器和 Qt 前缀。Qt 安装不含独立 protoc，以下脚本将官方 protoc 29.3 下载到 `client/.tools/`，不进行全局安装：

```powershell
Set-Location client
./tools/fetch_protoc.ps1
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --preset qt-windows
& "D:/software/Qt/Tools/CMake_64/bin/cmake.exe" --build --preset qt-windows -j 4
& "D:/software/Qt/Tools/CMake_64/bin/ctest.exe" --preset qt-windows
```

其他环境可在 PATH 提供 protoc，或配置时设置：

```sh
cmake -S client -B client/build -G Ninja \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.11.2/gcc_64 \
  -DCLIENT_PROTOC_EXECUTABLE=/opt/protobuf/bin/protoc
cmake --build client/build
ctest --test-dir client/build --output-on-failure
```

## 运行

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
client/build/qt-windows/bin/secure-im-client.exe `
  --data-dir D:/im-data/alice --server-url http://127.0.0.1:8080 --username alice
client/build/qt-windows/bin/secure-im-client.exe `
  --data-dir D:/im-data/bob --server-url http://127.0.0.1:8080 --username bob
```

`--data-dir` 必填且必须不是文件系统根目录；两个用户切勿共用，客户端会通过 `config/client.lock` 拒绝并发使用同一目录。`--username` 只预填内嵌登录页，可省略；已有有效会话文件时直接恢复聊天页。

多用户工作台：

```powershell
client/start-simulator.ps1 -ServerUrl http://127.0.0.1:8080
```

simulator 固定使用仓库下的 `runtime/users/user_N`，欢迎页以只读输入框显示该路径；编号单调递增，可用 `SECURE_IM_SERVER_URL` 设置默认服务器地址。关闭标签会幂等停止该用户的定时器、WebSocket 重连、HTTP、TransferManager 和 writer，并立即释放目录锁；其他标签不受影响，之后可重新打开同一目录。“退出”需确认并额外清除本地登录会话，但不删除服务器账号或历史文件；“注销”则在服务端永久删除账号成功后才执行相同的本地清理。注销请求期间关闭标签会安全中止回调，不影响其他标签。

## 后端对齐点

所有 REST 响应假设均集中在 `ApiClient.cpp`，UI 不直接读取后端字段。当前适配如下：

| 接口 | 当前请求/响应约定 |
|---|---|
| `POST /api/v1/auth/register`、`login` | JSON `username/password`，注册附带默认 `display_name=username`；读取 `data.user` 和 `data.tokens.access_token/refresh_token`；密码为 6~128 个 UTF-8 字节（`123456` 可用于本地测试） |
| `POST /api/v1/auth/refresh` | JSON `refresh_token`；响应同登录 |
| `GET /api/v1/users` | 数组或 `users` 数组；用户 `id`（兼容 `user_id`）、`username`、`display_name` |
| `GET /api/v1/presence` | 读取 `data.online_user_ids`，与用户列表共用 10 秒刷新周期；失败不改变旧状态 |
| `POST /api/v1/ws-ticket` | 读取 `ticket`（兼容 `ws_ticket`），随后连接 `ws(s)://host/ws?ticket=...` |
| `GET /api/v1/messages?peer_id=&after_id=` | `data.messages` 中的 `cursor` 用于下次 `after_id`；`envelope` 为 base64 Protobuf Envelope，payload 为 UTF-8 JSON，MESSAGE 的 `read_at` 持久化为已读时间 |
| `POST /api/v1/messages/read` | JSON `peer_id/cursor`；只为可见会话的最新对方消息发送，服务端随后通过 WS READ 通知发送方 |
| `POST /api/v1/files/init` | `recipient_id/filename/size/sha256/chunk_size`；读取 `data.file.id/chunk_size` 等 |
| `GET /api/v1/files/{id}` | 读取 `data.file` 及与其同级的 `uploaded_chunks`；完成状态为 `completed` |
| `PUT /api/v1/files/{id}/chunks/{index}` | 原始字节及 `X-Chunk-SHA256`，索引从 0 开始 |
| `POST /api/v1/files/{id}/complete` | 空 JSON 对象 |
| `GET /api/v1/files/{id}/download` | `readyRead` 流式读取，writer 校验实际大小后通过 `QSaveFile` 原子提交 |
| `DELETE /api/v1/users/me` | Bearer 认证；永久删除当前服务器账号及关联消息/文件；**仅 204 视为成功**，其他 2xx 也按失败处理 |
| `DELETE /api/v1/files/{id}` | 取消上传时调用 |

通用响应可包一层 `data`。`uploaded_chunks` 可为整数数组，或带 `index/uploaded/complete` 的对象数组。错误响应读取嵌套 `error.message`（兼容 `error/code/message/detail`）。若后端最终字段不同，只需调整 `ApiClient.cpp`。

`tests/test_api_client.cpp` 使用本地假 HTTP 服务固定上述真实响应形状，包括 presence、read 和账号注销的 Bearer/204 严格判定。`test_message_sync.cpp` 覆盖 optimistic 去重及 cursor 单调推进；`test_local_store.cpp` 覆盖重复 history 的 cursor/readAt 合并。`test_transfer_manager.cpp` 覆盖 8 MiB 流式下载、大小不匹配清理和暂停后继续。`test_session_tabs.cpp` 覆盖系统测试/欢迎固定标签顺序与关闭按钮、用户标签关闭和切换、目录/token/对象隔离、锁拒绝与释放、固定 `runtime/users`、大写登录和 6 位密码、退出与注销的确认/取消、`session.json` 清除、DELETE 成功/失败与重试、防重入、pending 请求关闭安全、成功后关闭标签并回到欢迎页、其他标签及历史文件保留、fake 目录 opener（不会启动 Explorer）、两个联系人独立未读红点及分联系人清除、搜索/刷新后的未读保留、UUID 隐藏、后台标签不误已读、切换补发及 READ 持久化，以及未完成 HTTP 和活动上传/下载的关闭安全。

`session_tabs` 还在 `QT_QPA_PLATFORM=offscreen` 下实际生成并校验非空 PNG（Windows CTest 自动设置 `QT_QPA_FONTDIR=C:/Windows/Fonts`）：

- `build/qt-windows/system-test-preview.png`：固定在首个标签的系统测试预留页面。
- `build/qt-windows/session-tabs-welcome-preview.png`：与系统测试并列的欢迎标签。
- `build/qt-windows/session-tabs-preview.png`：两个用户标签、登录态及传输视图，右上角展示连接状态与“退出”“注销”，联系人姓名旁展示未读红点。
- `build/qt-windows/session-tabs-chat-preview.png`：同一工作台的聊天视图，展示大写用户名、在线状态、右上角“退出”“注销”、隐藏 UUID 的标题和自己消息的已读时间。

也可手动生成传输视图预览：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:QT_QPA_PLATFORM = "offscreen"
$env:QT_QPA_FONTDIR = "C:/Windows/Fonts"
client/build/qt-windows/bin/secure-im-simulator.exe `
  --preview client/build/qt-windows/session-tabs-preview.png
```

`test_backend_integration` 不注册到默认 CTest，可对运行中的后端执行随机大写双用户使用 `123456` 注册、登录、users/presence、双 WebSocket 连接、文本发送、ACK、对方实时接收、READ 回执、history `read_at` 及本地持久化和 cursor 增量，并验证文件 init/status、分块上传、completed 状态、流式下载，最后通过新 DELETE 接口注销两个测试账号：

```powershell
$env:PATH = "D:/software/Qt/6.11.2/llvm-mingw_64/bin;$env:PATH"
$env:CLIENT_BACKEND_URL = "http://127.0.0.1:8080"
client/build/qt-windows/test_backend_integration.exe
```

WebSocket 使用仓库现有 `im.v1.Envelope`。因 `proto/im/v1/messages.proto` 尚未提供，`Envelope.payload` 暂时携带 UTF-8 JSON：文本为 `{"type":"text","text":"..."}`，文件为 `{"type":"file","file_id":"...","file_name":"...","size":123}`。这是**唯一需要随消息 proto 定稿对齐的 payload 适配点**；外层帧始终由生成的 Protobuf 代码处理。

当前范围刻意不包含 QML、群聊和 E2EE。下载接口未约定 Range/分块响应，所以暂停后继续下载会重新获取完整文件；上传暂停后继续会查询并保留服务端已确认的分块。
