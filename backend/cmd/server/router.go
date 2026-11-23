package main

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/httpx"
)

// router exposes the public API and an authenticated subgroup. WebSocket is
// dispatched from NoRoute because its historical methodless route accepts
// non-standard HTTP methods as well as the usual GET upgrade.
type router struct {
	engine    *gin.Engine
	public    *gin.RouterGroup
	protected *gin.RouterGroup
}

func newRouter(authenticate, webSocket gin.HandlerFunc, middleware ...gin.HandlerFunc) *router {
	engine := gin.New()
	// The explicit JSON 404 is part of the API; Gin must not replace it with a
	// redirect or its own plain-text error response.
	engine.RedirectTrailingSlash = false
	engine.RedirectFixedPath = false
	engine.Use(middleware...)
	engine.NoRoute(func(c *gin.Context) {
		if c.Request.URL.Path == "/ws" && webSocket != nil {
			webSocket(c)
			return
		}
		httpx.Fail(c, httpx.Error(http.StatusNotFound, "not_found", "route not found"))
	})
	public := engine.Group("/api/v1")
	return &router{engine: engine, public: public, protected: public.Group("", authenticate)}
}

func (r *router) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	r.engine.ServeHTTP(w, req)
}

func get(routes gin.IRoutes, path string, handler gin.HandlerFunc) {
	routes.Handle(http.MethodGet, path, handler)
	// GET routes explicitly retain net/http ServeMux's HEAD behavior.
	routes.Handle(http.MethodHead, path, handler)
}
