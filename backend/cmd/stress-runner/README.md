# stress-runner

Real Secure IM load generator. It provisions one `STRESS_*` user per connection with a single reused Argon2id hash, signs access tokens with the backend `auth.TokenManager`, then uses real ws-ticket/WebSocket Protobuf traffic and file REST APIs. Preparation is excluded from the measured stable phase.

From the repository root:

```powershell
go -C backend build -o .cache/stress-runner.exe ./cmd/stress-runner
backend/.cache/stress-runner.exe
```

The defaults are 1000 users, an 80/18/2 idle/chat/file mix, 200 connections/s, ten minutes stable, 0.05 messages/s/chat user, one 100 MiB file per file user with 1 MiB chunks, 64 file workers and a 500 MiB backend file-budget criterion. The default server is `http://127.0.0.1:18080`; reports go to `runtime/stress-reports/<run-id>`.

For the isolated Docker stress stack, pass `--env-file deploy/.env.stress`. The runner reads the sibling `deploy/.env` for root MySQL/MinIO credentials and uses `secure_im_stress` plus `secure-im-stress-files`. It deletes only its own run's objects and rows, never either database or bucket itself.

Use `--help` for public overrides. Hidden integration overrides are `--duration=5s`, `--file-mib=0.01`, and `--file-size-bytes=65536`; a one-chunk file smaller than the configured chunk size is valid.

Stdout is NDJSON only. Unknown backend/docker measurements are JSON `null`, never invented values. Exit code is zero for `success` and `fluctuation`, and nonzero for `failure` after reports and cleanup have been attempted.
