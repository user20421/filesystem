package main

import "secure-im/backend/internal/httpx"

// routes 只负责组合中间件和注册路由，不承载装配或生命周期逻辑。
func (a *application) routes() *router {
	mux := newRouter(a.authHTTP.Middleware(), a.chatHTTP.WebSocket, httpx.RequestContext(), httpx.Recover(a.log), requestLogger(a.log))
	get(mux.engine, "/health", healthHandler)
	get(mux.engine, "/ready", readinessHandler(a.db, a.objects))
	if a.cfg.EnableDebugStats {
		get(mux.engine, "/debug/stats", debugStatsHandler(a.files))
	}

	mux.public.POST("/auth/register", a.authHTTP.Register)
	mux.public.POST("/auth/login", a.authHTTP.Login)
	mux.public.POST("/auth/refresh", a.authHTTP.Refresh)
	mux.protected.POST("/auth/logout", a.authHTTP.Logout)
	get(mux.protected, "/users/me", a.authHTTP.Me)
	mux.protected.DELETE("/users/me", a.accountHTTP.DeleteMe)
	get(mux.protected, "/users", a.authHTTP.Users)
	get(mux.protected, "/presence", a.chatHTTP.Presence)
	mux.protected.POST("/ws-ticket", a.chatHTTP.Ticket)
	get(mux.protected, "/messages", a.chatHTTP.History)
	get(mux.protected, "/messages/unread", a.chatHTTP.Unread)
	mux.protected.POST("/messages/read", a.chatHTTP.MarkRead)
	mux.protected.POST("/files/init", a.fileHTTP.Init)
	get(mux.protected, "/files/:file_id", a.fileHTTP.Status)
	mux.protected.PUT("/files/:file_id/chunks/:index", a.fileHTTP.Upload)
	mux.protected.POST("/files/:file_id/complete", a.fileHTTP.Complete)
	get(mux.protected, "/files/:file_id/download", a.fileHTTP.Download)
	mux.protected.DELETE("/files/:file_id", a.fileHTTP.Delete)
	return mux
}
