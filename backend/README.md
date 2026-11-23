# Secure IM Go backend

A modular monolith providing authenticated direct messaging over WebSocket and chunked file transfer. It uses MySQL for metadata, MinIO for plaintext chunks, and an in-memory single-instance online hub. Only MySQL and MinIO are required infrastructure.

## Run locally

Requirements: Go 1.27+, MySQL 8, and a MinIO-compatible S3 service. Generated Protobuf Go code is committed; `protoc` is not needed to build or test.

1. Create the database `secure_im` (or set `MYSQL_DATABASE` / `MYSQL_DSN`).
2. Copy `.env.example` values into your shell environment. The server does not implicitly read `.env` files.
3. Set `JWT_SECRET` to at least 32 random bytes. Never commit production values. No file-encryption key is configured because objects are stored as plaintext chunks.
4. Start the server:

```powershell
go run ./cmd/server
```

The default listener is `127.0.0.1:8080`. Startup pings MySQL, applies embedded SQL migrations, ensures the MinIO bucket exists, and starts the file cleanup sweeper. `GET /health` is liveness; `GET /ready` checks MySQL and object storage. SIGINT/SIGTERM closes WebSocket clients and gracefully drains HTTP requests. Shutdown cancels the sweeper and waits for its current round before closing MySQL.

MySQL can be configured with `MYSQL_DSN`, or with `MYSQL_ADDR`, `MYSQL_DATABASE`, `MYSQL_USER`, and `MYSQL_PASSWORD`; an explicit DSN takes precedence. MinIO defaults to `127.0.0.1:9000` and bucket `secure-im-files`; credentials come from `MINIO_ACCESS_KEY` and `MINIO_SECRET_KEY`, with `MINIO_USE_SSL` selecting HTTPS.

Current server defaults are `LISTEN_ADDR=127.0.0.1:8080`, `ACCESS_TOKEN_TTL=15m`, `REFRESH_TOKEN_TTL=720h`, `WS_TICKET_TTL=1m`, and `WS_MAX_CONNECTIONS=1000` (allowed range 1–100000). File sizes and memory budgets are bytes: `MAX_ACTIVE_FILES=3` (3–5), `MAX_CHUNK_SIZE=5242880` (64 KiB–16 MiB), `MAX_FILE_SIZE=5368709120`, `UPLOAD_CONCURRENCY=96` (1–256), and `FILE_MEMORY_BUDGET=524288000`. Background file cleanup defaults to `FILE_UPLOAD_IDLE_TTL=24h`, `FILE_DELETING_RETRY_AFTER=5m`, and `FILE_CLEANUP_INTERVAL=1m`; all three durations must be positive. `ENABLE_DEBUG_STATS` defaults to false. Auth limits default to 30 registrations/minute and 10 logins/minute with a 10000-key limiter cache. See `.env.example` for the complete process-environment configuration; malformed optional numeric, boolean, and duration values use their defaults, while values outside validated ranges are rejected.

## HTTP conventions

Successful JSON responses are `{ "data": ... }`. Errors are:

```json
{"error":{"code":"invalid_json","message":"invalid JSON request","request_id":"..."}}
```

`X-Request-ID` is propagated when valid, otherwise generated. Protected routes require `Authorization: Bearer <access_token>`. Access tokens are short-lived JWTs; refresh tokens are random 256-bit values, SHA256-hashed in MySQL, and rotated on every refresh.

### Authentication and users

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/api/v1/auth/register` | No | Register; password is hashed with Argon2id |
| POST | `/api/v1/auth/login` | No | Login with a generic failure error |
| POST | `/api/v1/auth/refresh` | No | Rotate a refresh token |
| POST | `/api/v1/auth/logout` | Yes | Revoke the supplied refresh token for the current user |
| GET | `/api/v1/users/me` | Yes | Current user |
| DELETE | `/api/v1/users/me` | Yes | Permanently delete the current account; success is 204 |
| GET | `/api/v1/users` | Yes | Up to 500 users ordered by username |

Register/login JSON: `username`, `display_name` (registration only), and `password`. Registration and login normalize usernames with Unicode uppercase mapping, and all returned usernames are uppercase. Registration passwords must contain **6 to 128 bytes**; UTF-8 multibyte characters count as their encoded bytes. Refresh JSON: `refresh_token`. Auth responses deliberately nest both values:

```json
{
  "data": {
    "user": {"id":"...","username":"ALICE","display_name":"Alice","created_at":"..."},
    "tokens": {"access_token":"...","refresh_token":"...","token_type":"Bearer","expires_in":900}
  }
}
```

The users list is `{ "data": { "users": [...] } }`.

Registration and login use a single-process fixed-window limiter. Registration defaults to 30 requests/minute per client IP; login defaults to 10 requests/minute per client IP and normalized username. Excess requests return HTTP 429 with code `rate_limited` and `Retry-After`. Configure `AUTH_REGISTER_RATE_LIMIT`, `AUTH_LOGIN_RATE_LIMIT`, `AUTH_RATE_LIMIT_WINDOW`, and `AUTH_RATE_LIMIT_MAX_KEYS`; setting an individual limit to zero disables it.

Logout requires Bearer auth plus JSON `refresh_token`, revokes that token only when it belongs to the current user, and returns 204 idempotently. Other login sessions are unaffected. Logout does not add a server-side denylist for the short-lived access token; permanent account deletion does invalidate access through the user lookup described below.

Account deletion removes the account, its refresh tokens, every conversation and message it participates in, and every logical file reference for which it is owner or recipient. Shared plaintext content remains available to other references; MinIO objects are removed only when the final reference is deleted. Other users' unrelated conversations and files are preserved. Last-reference object deletion uses the same per-file locks as normal transfer deletion and occurs before the account metadata transaction; MinIO is never called inside that transaction. A failure returns retryable `503 account_deletion_failed` without deleting the user row, so the client can retry with the same access token. After a successful 204, middleware rejects the old access token because the user no longer exists, refresh tokens fail, all of that user's WebSocket connections close, and all outstanding WebSocket tickets are revoked without affecting other users.

### Messaging

`GET /api/v1/presence` requires Bearer auth and returns `{data:{online_user_ids:[...]}}`. IDs are sorted lexicographically. A user is online while at least one WebSocket connection remains and goes offline when the last connection is removed.

1. `POST /api/v1/ws-ticket` with Bearer auth returns `{data:{ticket,expires_in}}`.
2. Connect to `/ws?ticket=...`. Tickets expire and are consumed on the first upgrade.
3. Send and receive **binary Protobuf** `im.v1.Envelope` frames from `../proto/im/v1/envelope.proto`. Generated Go types are in `internal/proto/impb`.

Client MESSAGE requirements: `version=2`, nonempty `message_id` (maximum 64 bytes), `recipient_id`, `device_id`, and 1 byte to 256 KiB of `payload`. `sender_id`, when supplied, must match the ticket user. MESSAGE payloads are `im.v1.MessagePayload` (`text` or `file`), ACK payloads are `im.v1.AckPayload`, and ERROR payloads are `im.v1.ErrorPayload`; legacy JSON and decimal-cursor payloads are not accepted. READ carries its fields directly in the Envelope. The server assigns the direct conversation and timestamp and persists before sending an ACK. Reusing the same sender/message ID with identical content returns the same ACK without another push; changed content is rejected with an ERROR frame.

The hub pushes to at most five connections per user, uses bounded queues, and disconnects slow consumers; missed messages remain available offline. Binary PING/PONG application envelopes and WebSocket ping/pong are supported. Frames are limited to 300 KiB and reads expire after 75 seconds without activity.

Fetch missed messages with:

```text
GET /api/v1/messages?peer_id=<user-id>&after_id=0&limit=200
Authorization: Bearer ...
```

Fetch all unread counts without loading each conversation's full history:

```text
GET /api/v1/messages/unread
Authorization: Bearer ...
```

The Bearer-protected endpoint groups unread incoming rows by `sender_id` for the authenticated `recipient_id` and returns `{data:{unread:[{peer_id,unread_count,last_message_at}]}}`. It includes only peers with unread messages, sorts entries by `peer_id`, and returns an empty array when there are none. `last_message_at` is the latest unread message creation time in Unix milliseconds. Clients use this single summary to restore contact unread indicators after login or session restoration; peers changed by realtime events while the request is in flight are not overwritten by the older snapshot.

History returns cursor plus base64-encoded binary Protobuf, not a direct payload field:

```json
{"data":{"messages":[{"cursor":42,"envelope":"CAoQAR..."}]}}
```

Decode the base64, then unmarshal an Envelope. `read_at` is Unix milliseconds and is zero while unread. Use the last cursor as `after_id` on the next request. `limit` defaults to 200 and ranges from 1 to 500.

Mark incoming messages read with Bearer auth:

```json
POST /api/v1/messages/read
{"peer_id":"sender-user-id","cursor":42}
```

The cursor must identify a message sent by `peer_id` to the current user; missing, unrelated and unauthorized cursors all receive the same `403 read_forbidden` response. In one transaction, the server marks all previously unread incoming messages in that conversation through the cursor, preserving the first `read_at`. The response is:

```json
{"data":{"conversation_id":"...","peer_id":"...","cursor":42,"read_at":1760000000000,"updated":2}}
```

`updated` counts newly marked rows, so an exact repeat returns the original `read_at` and `updated:0`. The server broadcasts a `READ` Envelope to every connection of both the original sender and reader. It has `sender_id=reader`, `recipient_id=original sender`, `conversation_id`, `cursor`, and matching `read_at`/`timestamp` in Unix milliseconds. READ events are not stored as messages; both participants see persisted message `read_at` in later offline history.

### Chunked file transfer and content reuse

| Method | Path | Caller | Purpose |
|---|---|---|---|
| POST | `/api/v1/files/init` | Sender | Create a logical reference or reuse ready content |
| PUT | `/api/v1/files/{file_id}/chunks/{index}` | Sender | Upload one raw plaintext chunk |
| GET | `/api/v1/files/{file_id}` | Sender or recipient | Metadata, `instant`, and uploaded indexes |
| POST | `/api/v1/files/{file_id}/complete` | Sender | Verify all plaintext and mark content ready |
| GET | `/api/v1/files/{file_id}/download` | Sender or recipient | Stream plaintext |
| DELETE | `/api/v1/files/{file_id}` | Sender | Cancel/delete a reference; remove objects after the last reference |

Initialize with the exact field names:

```json
{
  "filename": "report.pdf",
  "size": 150003,
  "sha256": "64-lowercase-or-uppercase-hex-characters",
  "recipient_id": "recipient-user-id",
  "chunk_size": 65536
}
```

`chunk_size=0` selects the default (**5 MiB**, capped by the server maximum). Chunks are zero-based and all except the last must be exactly `chunk_size`; the last must make the total exactly `size`. Upload raw bytes with `Content-Type: application/octet-stream` and `X-Chunk-SHA256: <chunk hex SHA256>`. Reuploading an index while active atomically replaces its metadata and uses a new object key. Same-content replacement and completion are serialized per content so completion never reads a deleted object key; different contents remain independent. If another reference completes shared content during an upload, a subsequent chunk request with the verified stored size and SHA256 succeeds idempotently.

Content identity is the pair `(size, sha256)`. If matching content is already ready, init creates another logical file reference and returns `instant=true`; the sender skips chunk upload and completion. Uploading content and existing references are not treated as ready. Shared content and its objects survive deletion until the final logical reference is removed.

A single-process, non-overlapping sweeper selects at most 100 file references per round, with a 30-second round deadline. Uploading content whose `updated_at` has been idle for `FILE_UPLOAD_IDLE_TTL` loses its active references; failed final deletions become eligible again after `FILE_DELETING_RETRY_AFTER`. Status and cutoff are checked again under the file/content locks, so activity after selection or a newly shared/ready state is skipped. Non-final reference removal preserves the content's idle timestamp, allowing the remaining stale references to be cleaned on subsequent rounds rather than restarting the TTL. Every round logs `selected`, `succeeded`, `skipped`, and `failed`; individual failures are logged and retried by later rounds.

Final-reference deletion holds the content lock while recursively listing only `contents/<content-id>/` and removing every returned key, including unregistered objects and replacement leftovers. It does not rely solely on `file_chunks`. Metadata is finalized only after List and all Remove calls succeed; List, Remove, or database failures leave the `deleting` state available for idempotent manual/background retry. An empty listing is sufficient to finalize metadata. While cleanup is pending, initializing identical content returns `content_cleanup_pending` instead of attaching a new reference.

Status places `uploaded_chunks` beside `file`:

```json
{"data":{"file":{"id":"...","status":"uploading","chunk_count":3,"...":"..."},"uploaded_chunks":[0,1,2]}}
```

Completion reads each plaintext chunk, verifies its chunk hash and the assembled whole-file SHA256, then marks the shared content ready. Only completed logical files can be downloaded. Download headers include `Content-Length` and RFC 5987 `Content-Disposition`.

MinIO stores the same plaintext chunk bytes received by the upload API; there is no DEK, KEK, `FILE_KEK`, chunk encryption, or decryption step. Download and Complete clear the server's fixed whole-request write deadline for the current operation; download applies a rolling 30-second idle write timeout before each payload write, so slow but continuously progressing transfers are not cut off at 60 seconds. Upload, completion, and download share a context-aware fair chunk scheduler. Each upload request takes one slot; completion and download re-enter the FIFO queue for every chunk instead of holding a slot for the whole file. When multiple files compete, each holds at most one slot and a newly arriving file takes the next available turn; a single file may use multiple idle slots. Download writes and completion hashes remain in chunk order, and a blocked download write retains its chunk reservation for backpressure.

`UPLOAD_CONCURRENCY` is the configured upper bound for these shared chunk slots (default 96, maximum 256). The effective concurrency is also bounded by queued work and `FILE_MEMORY_BUDGET`: the budget defaults to 500 MiB and reserves each plaintext buffer plus per-chunk overhead across all three operations. When memory is full, chunks wait on their request context and reservations never exceed the budget. The default active-upload limit remains three per user (configurable from three to five). For the isolated stress backend, set `STRESS_UPLOAD_CONCURRENCY` to override the default slot cap of 96 without changing the 500 MiB memory ceiling. The 96 default was selected from local 16-step benchmarks through 128 with 128 12-MiB file jobs and a fixed 500-MiB budget; all tested tiers completed without file failures, while higher tiers did not provide a stable throughput improvement.

Set `ENABLE_DEBUG_STATS=true` to expose unauthenticated `GET /debug/stats`, containing Go heap/runtime statistics, goroutine count, GOMAXPROCS, `file_memory_budget.used`/`limit` bytes, and `file_chunk_slots.used`/`limit`. Chunk slots are a thread-safe snapshot of the shared scheduler's actual granted operations and configured cap (96 by default in both normal and stress configurations). Only enable it with a loopback-only host port mapping.

## Tests

Unit tests cover Argon2id, access/refresh tokens, register/login rate limits, logout revocation, deleted-user access rejection, Protobuf v2 payload validation, one-time and per-user WebSocket ticket revocation, targeted hub closure, account deletion ordering/retry, permissions, content reuse, chunk boundaries, deterministic fair-chunk rotation/single-file parallelism/cancellation/wakeup/limits, concurrent file-memory reservation/release/timeout, and file cleanup freshness/rechecks/shared-content safety/orphan removal/retry/empty-list finalization/cancellation/non-overlap/manual-delete races:

```powershell
go test ./...
go test -race ./...
go vet ./...
```

Repeatable live integration coverage is in `integration_test.go`, `integration_read_test.go`, `integration_auth_test.go`, and `integration_account_test.go`; the top-level tests are skipped by default. `integration_read_test.go` supplies the READ/unread checks invoked by `TestRealAPIIntegration`, while `integration_auth_test.go` contains the standalone logout/session test. With a running backend, MySQL, and MinIO credentials:

```powershell
$env:SECURE_IM_INTEGRATION_URL = 'http://127.0.0.1:8080'
$env:INTEGRATION_MYSQL_DSN = 'secure_im:...@tcp(127.0.0.1:13306)/secure_im?parseTime=true&loc=UTC&time_zone=%27%2B00:00%27'
$env:INTEGRATION_MINIO_ENDPOINT = '127.0.0.1:19000'
$env:INTEGRATION_MINIO_ACCESS_KEY = '...'
$env:INTEGRATION_MINIO_SECRET_KEY = '...'
$env:INTEGRATION_MINIO_BUCKET = 'secure-im-files'
go test -run '^TestReal' -v .
```

They create unique users on every run and verify mixed-case registration/login, uppercase user listings, refresh rotation, per-session logout revocation and idempotency, cross-user logout denial, multi-device presence, bidirectional Protobuf v2 messaging, typed ACK/ERROR payloads and idempotency, grouped unread summaries before and after READ, batched READ delivery to both users' devices, repeat idempotency, third-party denial, persisted offline `read_at` with later messages left unread, multi-chunk transfer and SHA256, size+SHA256 instant reuse, plaintext in MinIO, and the active-file limit. The account test additionally verifies permanent deletion in MySQL and MinIO for sent and received files, shared-content last-reference cleanup, bidirectional conversation cleanup, old access/refresh token rejection, WebSocket closure and ticket revocation, while preserving another conversation, its users, and unrelated downloadable content.

To keep Go artifacts local, set `GOMODCACHE`, `GOCACHE`, and `GOPATH` under `backend/.cache`. The sources of truth for the wire protocol are `proto/im/v1/envelope.proto` and `proto/im/v1/messages.proto`; their generated Go files are committed so normal builds do not need a global `protoc`.

## Isolated stress backend

With the normal MySQL and MinIO containers healthy, run `scripts/stress-up.ps1`. It creates ignored `deploy/.env.stress` credentials, idempotently creates/grants only `secure_im_stress`, builds the backend image, and starts the independent `secure-im-stress` Compose project. The backend uses bucket `secure-im-stress-files`, **5 MiB** chunks, 96 shared chunk slots, **6 CPUs** (`cpus: 6` and `GOMAXPROCS=6`), a 3 GiB container limit, a 500 MiB file-memory budget, and is available only at `http://127.0.0.1:18080`. Register/login limits are disabled only in this isolated stress configuration. The ROOT/123456 account is created and verified solely for the local stress tool. `scripts/stress-down.ps1` removes only that backend project; it does not stop the shared base containers or delete any volumes or stress data.
