# Secure IM Go backend

A modular monolith providing authenticated direct messaging over WebSocket and encrypted chunked file transfer. It uses MySQL for metadata, MinIO for ciphertext, and an in-memory single-instance online hub. Redis is not required for correctness.

## Run locally

Requirements: Go 1.27+, MySQL 8, and a MinIO-compatible S3 service. Generated Protobuf Go code is committed; `protoc` is not needed to build or test.

1. Create the database `secure_im` (or set `MYSQL_DATABASE` / `MYSQL_DSN`).
2. Copy `.env.example` values into your shell environment. The server does not implicitly read `.env` files.
3. Set `JWT_SECRET` to at least 32 random bytes and `FILE_KEK` to the base64 encoding of exactly 32 random bytes. Never commit production values.
4. Start the server:

```powershell
go run ./cmd/server
```

The default listener is `127.0.0.1:8080`. Startup pings MySQL, applies embedded SQL migrations, and ensures the MinIO bucket exists. `GET /health` is liveness; `GET /ready` checks MySQL and object storage. SIGINT/SIGTERM closes WebSocket clients and gracefully drains HTTP requests.

MySQL can be configured with `MYSQL_DSN`, or with `MYSQL_ADDR`, `MYSQL_DATABASE`, `MYSQL_USER`, and `MYSQL_PASSWORD`. MinIO defaults to `127.0.0.1:9000` and bucket `secure-im-files`; credentials come from `MINIO_ACCESS_KEY` and `MINIO_SECRET_KEY`. See `.env.example` for all limits and TTLs.

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
| GET | `/api/v1/users/me` | Yes | Current user |
| DELETE | `/api/v1/users/me` | Yes | Permanently delete the current account; success is 204 |
| GET | `/api/v1/users` | Yes | Up to 500 users ordered by username |

Register/login JSON: `username`, `display_name` (registration only), and `password`. Registration and login normalize usernames with Unicode uppercase mapping, and all returned usernames are uppercase. Registration passwords must contain **6 to 128 bytes** (so `123456` is valid); UTF-8 multibyte characters count as their encoded bytes. Refresh JSON: `refresh_token`. Auth responses deliberately nest both values:

```json
{
  "data": {
    "user": {"id":"...","username":"ALICE","display_name":"Alice","created_at":"..."},
    "tokens": {"access_token":"...","refresh_token":"...","token_type":"Bearer","expires_in":900}
  }
}
```

The users list is `{ "data": { "users": [...] } }`.

Account deletion removes the account, its refresh tokens, every conversation and message it participates in, and every file for which it is owner or recipient, including all ciphertext objects in MinIO. Other users' unrelated conversations and files are preserved. File objects are deleted under the same per-file locks used by normal transfer deletion and before the account metadata transaction; MinIO is never called inside that transaction. A failure returns retryable `503 account_deletion_failed` without deleting the user row, so the client can retry with the same access token. After a successful 204, middleware rejects the old access token because the user no longer exists, refresh tokens fail, all of that user's WebSocket connections close, and all outstanding WebSocket tickets are revoked without affecting other users.

### Messaging

`GET /api/v1/presence` requires Bearer auth and returns `{data:{online_user_ids:[...]}}`. IDs are sorted lexicographically. A user is online while at least one WebSocket connection remains and goes offline when the last connection is removed.

1. `POST /api/v1/ws-ticket` with Bearer auth returns `{data:{ticket,expires_in}}`.
2. Connect to `/ws?ticket=...`. Tickets expire and are consumed on the first upgrade.
3. Send and receive **binary Protobuf** `im.v1.Envelope` frames from `../proto/im/v1/envelope.proto`. Generated Go types are in `internal/proto/impb`.

Client MESSAGE requirements: `version=1`, nonempty `message_id` (maximum 64 bytes), `recipient_id`, `device_id`, and 1 byte to 256 KiB of `payload`. `sender_id`, when supplied, must match the ticket user. The server assigns the direct conversation and timestamp. It persists before sending an ACK. ACK `payload` is the decimal database cursor. Reusing the same sender/message ID with identical content returns the same ACK without another push; changed content is rejected with an ERROR frame.

The hub pushes to at most five connections per user, uses bounded queues, and disconnects slow consumers; missed messages remain available offline. Binary PING/PONG application envelopes and WebSocket ping/pong are supported. Frames are limited to 300 KiB and reads expire after 75 seconds without activity.

Fetch missed messages with:

```text
GET /api/v1/messages?peer_id=<user-id>&after_id=0&limit=200
Authorization: Bearer ...
```

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

### Encrypted file transfer

| Method | Path | Caller | Purpose |
|---|---|---|---|
| POST | `/api/v1/files/init` | Sender | Create transfer and wrapped DEK |
| PUT | `/api/v1/files/{file_id}/chunks/{index}` | Sender | Upload one raw plaintext chunk |
| GET | `/api/v1/files/{file_id}` | Sender or recipient | Metadata and uploaded indexes |
| POST | `/api/v1/files/{file_id}/complete` | Sender | Verify all plaintext and mark complete |
| GET | `/api/v1/files/{file_id}/download` | Sender or recipient | Stream decrypted plaintext |
| DELETE | `/api/v1/files/{file_id}` | Sender | Cancel/delete and remove ciphertext |

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

`chunk_size=0` selects the default (1 MiB, capped by the server maximum). Chunks are zero-based and all except the last must be exactly `chunk_size`; the last must make the total exactly `size`. Upload raw bytes with `Content-Type: application/octet-stream` and `X-Chunk-SHA256: <chunk hex SHA256>`. Reuploading an index while active atomically replaces its metadata and uses a new object key.

Status places `uploaded_chunks` beside `file`:

```json
{"data":{"file":{"id":"...","status":"uploading","chunk_count":3,"...":"..."},"uploaded_chunks":[0,1,2]}}
```

Completion downloads and decrypts each ciphertext chunk, verifies each chunk hash and the assembled whole-file SHA256, then changes status to `completed`. Only completed files can be downloaded. Download headers include `Content-Length` and RFC 5987 `Content-Disposition`.

Each file has a random 32-byte DEK. Every chunk uses AES-256-GCM with an independent random nonce. The DEK is AES-256-GCM-wrapped using `FILE_KEK`; only wrapped keys and nonces are stored in MySQL, and only ciphertext is sent to MinIO. Plaintext chunks are bounded in memory, completion/download process one chunk at a time, and a context-aware semaphore bounds concurrent file operations. The default active-upload limit is three per user (configurable from three to five).

## Tests

Unit tests cover Argon2id, access/refresh tokens, deleted-user access rejection, one-time and per-user WebSocket ticket revocation, targeted hub closure, account deletion ordering/retry, AES key wrapping/chunk encryption, permissions, and chunk boundaries:

```powershell
go test ./...
go vet ./...
```

Repeatable live integration tests are in `integration_test.go`, `integration_read_test.go`, and `integration_account_test.go`; they are skipped by default. With a running backend, MySQL, and MinIO credentials:

```powershell
$env:SECURE_IM_INTEGRATION_URL = 'http://127.0.0.1:8080'
$env:INTEGRATION_MYSQL_DSN = 'secure_im:...@tcp(127.0.0.1:13306)/secure_im?parseTime=true'
$env:INTEGRATION_MINIO_ENDPOINT = '127.0.0.1:19000'
$env:INTEGRATION_MINIO_ACCESS_KEY = '...'
$env:INTEGRATION_MINIO_SECRET_KEY = '...'
$env:INTEGRATION_MINIO_BUCKET = 'secure-im-files'
go test -run 'TestReal(APIIntegration|AccountDeletionIntegration)' -v .
```

They create unique users on every run and verify mixed-case registration/login (including the six-byte password `123456`), uppercase user listings, refresh rotation, multi-device presence, bidirectional Protobuf messaging, ACK and idempotency, batched READ delivery to both users' devices, repeat idempotency, third-party denial, persisted offline `read_at` with later messages left unread, multi-chunk transfer and SHA256, ciphertext in MinIO, and the active-file limit. The account test additionally verifies permanent deletion in MySQL and MinIO for sent and received files, bidirectional conversation cleanup, old access/refresh token rejection, WebSocket closure and ticket revocation, while preserving another conversation, its users, and an unrelated downloadable file.

To keep Go artifacts local, set `GOMODCACHE`, `GOCACHE`, and `GOPATH` under `backend/.cache`. The source of truth for the wire protocol is `proto/im/v1/envelope.proto`; its generated Go file is committed so normal builds do not need a global `protoc`.
