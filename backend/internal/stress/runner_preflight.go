package stress

import (
	"context"
	"database/sql"
	"time"

	"github.com/minio/minio-go/v7"
	"secure-im/backend/internal/auth"
)

// preflightResult 保存后续 load/cleanup 阶段需要的真实资源。
type preflightResult struct {
	env            Environment
	db             *sql.DB
	objects        *minio.Client
	api            *APIClient
	users          []User
	resourcesReady bool
}

// preflight 按环境、HTTP、DB/Bucket、用户和 WebSocket 的顺序完成检查。
func (r *Runner) preflight(ctx context.Context, err error) (preflightResult, error) {
	var result preflightResult
	if err == nil {
		result.env, err = LoadEnvironment(r.cfg.EnvFile)
		if err != nil {
			r.fail("preflight: %v", err)
		}
	}
	result.api = NewAPIClient(r.cfg.Server)
	if err == nil {
		preflightCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		err = result.api.Health(preflightCtx, "/health")
		if err == nil {
			err = result.api.Health(preflightCtx, "/ready")
		}
		cancel()
		if err != nil {
			r.fail("preflight HTTP: %v", err)
		}
	}
	if err == nil {
		resourceCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
		result.db, result.objects, err = OpenResources(resourceCtx, result.env)
		cancel()
		if err != nil {
			r.fail("preflight DB/Bucket: %v", err)
		}
	}
	result.resourcesReady = result.db != nil && result.objects != nil
	if err == nil {
		tokens := auth.NewTokenManager(result.env.JWTSecret, 24*time.Hour, 24*time.Hour)
		prepareCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
		result.users, err = CreateUsers(prepareCtx, result.db, tokens, r.cfg, r.allocation)
		cancel()
		if err != nil {
			r.fail("prepare users: %v", err)
		} else if err = preflightWebSocket(ctx, result.api, result.users[0]); err != nil {
			r.fail("preflight ws-ticket/WebSocket: %v", err)
		}
	}
	return result, err
}

// preflightWebSocket 在正式 ramp-up 前验证票据和 WebSocket 建连。
func preflightWebSocket(ctx context.Context, api *APIClient, user User) error {
	preflightCtx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	ticket, err := api.Ticket(preflightCtx, user.Token)
	if err != nil {
		return err
	}
	conn, err := api.WebSocket(preflightCtx, ticket)
	if err != nil {
		return err
	}
	return conn.Close()
}
