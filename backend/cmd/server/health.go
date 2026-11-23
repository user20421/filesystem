package main

import (
	"context"
	"database/sql"
	"net/http"
	"runtime"
	"time"

	"github.com/gin-gonic/gin"
	"secure-im/backend/internal/httpx"
)

func healthHandler(c *gin.Context) {
	httpx.Data(c, http.StatusOK, map[string]string{"status": "ok"})
}

type readinessStore interface {
	Ensure(context.Context) error
}

// readinessHandler 分别检查数据库与对象存储，保持原有错误码和超时语义。
func readinessHandler(db *sql.DB, objects readinessStore) gin.HandlerFunc {
	return func(c *gin.Context) {
		readyCtx, cancel := context.WithTimeout(c.Request.Context(), 3*time.Second)
		defer cancel()
		if err := db.PingContext(readyCtx); err != nil {
			httpx.Fail(c, httpx.Error(http.StatusServiceUnavailable, "database_unavailable", "database is unavailable"))
			return
		}
		if err := objects.Ensure(readyCtx); err != nil {
			httpx.Fail(c, httpx.Error(http.StatusServiceUnavailable, "storage_unavailable", "object storage is unavailable"))
			return
		}
		httpx.Data(c, http.StatusOK, map[string]string{"status": "ready"})
	}
}

// debugStatsHandler 暴露进程和文件传输资源快照，路由是否注册仍由配置决定。
func debugStatsHandler(files interface {
	MemoryStats() (int64, int64)
	ChunkSlotStats() (int, int)
}) gin.HandlerFunc {
	return func(c *gin.Context) {
		var mem runtime.MemStats
		runtime.ReadMemStats(&mem)
		used, limit := files.MemoryStats()
		slotsUsed, slotsLimit := files.ChunkSlotStats()
		httpx.Data(c, http.StatusOK, map[string]any{
			"go": map[string]any{
				"mem_alloc_bytes":       mem.Alloc,
				"mem_total_alloc_bytes": mem.TotalAlloc,
				"mem_sys_bytes":         mem.Sys,
				"heap_alloc_bytes":      mem.HeapAlloc,
				"heap_inuse_bytes":      mem.HeapInuse,
				"heap_objects":          mem.HeapObjects,
				"goroutines":            runtime.NumGoroutine(),
				"gomaxprocs":            runtime.GOMAXPROCS(0),
			},
			"file_memory_budget": map[string]int64{"used": used, "limit": limit},
			"file_chunk_slots":   map[string]int{"used": slotsUsed, "limit": slotsLimit},
		})
	}
}
