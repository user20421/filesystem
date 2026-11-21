package stress

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/google/uuid"
)

type fileJob struct {
	id                string
	sender, recipient User
	queuedAt          time.Time
}
type fileInfo struct {
	ID         string `json:"id"`
	Size       int64  `json:"size"`
	SHA256     string `json:"sha256"`
	ChunkSize  int64  `json:"chunk_size"`
	ChunkCount int    `json:"chunk_count"`
	Status     string `json:"status"`
	Instant    bool   `json:"instant"`
}

type FilePool struct {
	cfg                Config
	api                *APIClient
	metrics            *Metrics
	budget             *ByteBudget
	queue              chan fileJob
	workers, producers sync.WaitGroup
	ownerLocks         []sync.Mutex
	dataError          func(error)
	failure            func(error)
	transferFunc       func(context.Context, fileJob) error
}

func NewFilePool(cfg Config, api *APIClient, metrics *Metrics, userCount int, dataError func(error), callbacks ...func(error)) *FilePool {
	var failure func(error)
	if len(callbacks) > 0 {
		failure = callbacks[0]
	}
	return &FilePool{cfg: cfg, api: api, metrics: metrics, budget: NewByteBudget(cfg.FileBudgetMiB << 20), queue: make(chan fileJob, cfg.FileQueueSize), ownerLocks: make([]sync.Mutex, userCount), dataError: dataError, failure: failure}
}
func (p *FilePool) Start(runCtx, produceCtx context.Context, users []User) {
	for i := 0; i < p.cfg.FileWorkers; i++ {
		p.workers.Add(1)
		go func() { defer p.workers.Done(); p.worker(runCtx) }()
	}
	for _, user := range users {
		if user.Role != "file" || user.Partner < 0 {
			continue
		}
		recipient := users[user.Partner]
		p.producers.Add(1)
		go func() { defer p.producers.Done(); p.produce(produceCtx, user, recipient) }()
	}
	go func() { p.producers.Wait(); close(p.queue) }()
}
func (p *FilePool) produce(ctx context.Context, sender, recipient User) {
	// One finite task per file user makes stable-phase completion and drain deterministic.
	job := fileJob{id: uuid.NewString(), sender: sender, recipient: recipient, queuedAt: time.Now()}
	p.metrics.FileQueued(1) // includes producers blocked by a full bounded queue
	select {
	case <-ctx.Done():
		p.metrics.FileQueued(-1)
	case p.queue <- job:
	}
}
func (p *FilePool) worker(ctx context.Context) {
	for job := range p.queue {
		p.metrics.FileQueued(-1)
		p.metrics.FileQueueWait(time.Since(job.queuedAt))
		p.metrics.FileActive(1)
		// Serializing per owner avoids consuming the server's three-active-file slots with stale jobs.
		owner := p.ownerLock(job.sender.ID)
		owner.Lock()
		var err error
		if ctx.Err() != nil {
			err = ctx.Err()
		} else if p.transferFunc != nil {
			err = p.transferFunc(ctx, job)
		} else {
			err = p.transfer(ctx, job)
		}
		owner.Unlock()
		if err != nil {
			p.metrics.FileFailed()
			if p.failure != nil {
				p.failure(fmt.Errorf("file %s sender %s: %w", job.id, job.sender.Username, err))
			}
		} else {
			p.metrics.FileCompleted(p.cfg.FileSize)
		}
		p.metrics.FileActive(-1)
	}
}
func (p *FilePool) ownerLock(id string) *sync.Mutex {
	// User IDs are UUIDs; a stable small hash avoids another map and its lock on the hot path.
	var h uint32
	for i := 0; i < len(id); i++ {
		h = h*33 + uint32(id[i])
	}
	return &p.ownerLocks[h%uint32(len(p.ownerLocks))]
}
func (p *FilePool) Wait()                                 { p.workers.Wait() }
func (p *FilePool) BudgetSnapshot() (int64, int64, int64) { return p.budget.Snapshot() }

func (p *FilePool) transfer(ctx context.Context, job fileJob) (result error) {
	const hashBufferSize = int64(128 << 10)
	if err := p.budget.Acquire(ctx, hashBufferSize); err != nil {
		return err
	}
	expected, err := syntheticHashBuffered(job.id, p.cfg.FileSize, make([]byte, int(hashBufferSize)))
	p.budget.Release(hashBufferSize)
	if err != nil {
		return err
	}
	input := map[string]any{"filename": "stress-" + job.id + ".bin", "size": p.cfg.FileSize, "sha256": expected, "recipient_id": job.recipient.ID, "chunk_size": p.cfg.ChunkSize}
	var info fileInfo
	if err = p.api.JSON(ctx, http.MethodPost, "/api/v1/files/init", job.sender.Token, input, &info, http.StatusCreated); err != nil {
		return err
	}
	defer func() {
		if result != nil {
			cleanupCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			_ = p.api.JSON(cleanupCtx, http.MethodDelete, "/api/v1/files/"+info.ID, job.sender.Token, nil, nil, http.StatusNoContent)
		}
	}()
	if !info.Instant && info.Status != "completed" {
		for index := 0; index < info.ChunkCount; index++ {
			size := info.ChunkSize
			if index == info.ChunkCount-1 {
				size = info.Size - int64(index)*info.ChunkSize
			}
			if err = p.budget.Acquire(ctx, size); err != nil {
				return err
			}
			chunk := make([]byte, int(size))
			fillSynthetic(chunk, int64(index)*info.ChunkSize, job.id)
			sum := sha256.Sum256(chunk)
			err = p.uploadChunk(ctx, job.sender.Token, info.ID, index, chunk, hex.EncodeToString(sum[:]))
			p.budget.Release(size)
			if err != nil {
				return err
			}
		}
		if err = p.api.JSON(ctx, http.MethodPost, "/api/v1/files/"+info.ID+"/complete", job.sender.Token, nil, &info, http.StatusOK); err != nil {
			return err
		}
	}
	downloaded, err := p.downloadHash(ctx, job.recipient.Token, info.ID, job.id)
	if err != nil {
		return err
	}
	if downloaded != expected {
		err = fmt.Errorf("download SHA256 mismatch: got %s want %s", downloaded, expected)
		p.dataError(err)
		return err
	}
	return nil
}

type uploadResponseError struct {
	status  int
	message string
}

func (e *uploadResponseError) Error() string { return e.message }

func (p *FilePool) uploadChunk(ctx context.Context, token, id string, index int, chunk []byte, sum string) error {
	var err error
	for attempt := 0; attempt < 4; attempt++ {
		err = p.uploadChunkOnce(ctx, token, id, index, chunk, sum)
		if err == nil || ctx.Err() != nil {
			return err
		}
		var responseErr *uploadResponseError
		if errors.As(err, &responseErr) && responseErr.status >= 400 && responseErr.status < 500 {
			return err
		}
		if attempt < 3 {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(time.Duration(50*(1<<attempt)) * time.Millisecond):
			}
		}
	}
	return err
}
func (p *FilePool) uploadChunkOnce(ctx context.Context, token, id string, index int, chunk []byte, sum string) error {
	path := "/api/v1/files/" + id + "/chunks/" + strconv.Itoa(index)
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, p.api.Base+path, bytes.NewReader(chunk))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("X-Chunk-SHA256", sum)
	req.ContentLength = int64(len(chunk))
	resp, err := p.api.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return &uploadResponseError{status: resp.StatusCode, message: fmt.Sprintf("upload chunk %d: HTTP %d: %s", index, resp.StatusCode, string(body))}
	}
	_, _ = io.Copy(io.Discard, resp.Body)
	return nil
}

func (p *FilePool) downloadHash(ctx context.Context, token, id, seed string) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.api.Base+"/api/v1/files/"+id+"/download", nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := p.api.HTTP.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return "", fmt.Errorf("download: HTTP %d: %s", resp.StatusCode, string(body))
	}
	if err = p.budget.Acquire(ctx, p.cfg.ChunkSize); err != nil {
		return "", err
	}
	defer p.budget.Release(p.cfg.ChunkSize)
	buffer := make([]byte, int(p.cfg.ChunkSize))
	hash := sha256.New()
	count, err := io.CopyBuffer(hash, resp.Body, buffer)
	if err != nil {
		return "", err
	}
	if count != p.cfg.FileSize {
		err = fmt.Errorf("download size mismatch: got %d want %d", count, p.cfg.FileSize)
		p.dataError(err)
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

type patternReader struct {
	offset, remaining int64
	seed              byte
}

func (r *patternReader) Read(dest []byte) (int, error) {
	if r.remaining == 0 {
		return 0, io.EOF
	}
	n := int64(len(dest))
	if n > r.remaining {
		n = r.remaining
	}
	for i := int64(0); i < n; i++ {
		dest[i] = byte((r.offset+i)*31) + r.seed
	}
	r.offset += n
	r.remaining -= n
	return int(n), nil
}
func seedByte(seed string) byte { sum := sha256.Sum256([]byte(seed)); return sum[0] }
func fillSynthetic(dest []byte, offset int64, seed string) {
	value := seedByte(seed)
	for i := range dest {
		dest[i] = byte((offset+int64(i))*31) + value
	}
}
func syntheticHash(seed string, size int64) (string, error) {
	return syntheticHashBuffered(seed, size, make([]byte, 128<<10))
}
func syntheticHashBuffered(seed string, size int64, buffer []byte) (string, error) {
	hash := sha256.New()
	_, err := io.CopyBuffer(hash, &patternReader{remaining: size, seed: seedByte(seed)}, buffer)
	if err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}
