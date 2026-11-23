package filetransfer

import (
	"context"
	"fmt"
	"io"

	"secure-im/backend/internal/apperror"
)

// Download 按顺序流式写出已完成文件，并为每个分块独立获取调度资源。
func (s *Service) Download(ctx context.Context, id, user string, output io.Writer, onMetadata func(File)) error {
	unlock := s.lock("file:" + id)
	defer unlock()
	f, err := s.getFile(ctx, id)
	if err != nil {
		return err
	}
	if err := Authorize(f, user, false); err != nil {
		return err
	}
	if f.Status != "completed" {
		return apperror.New(apperror.KindConflict, "file_not_ready", "only completed files can be downloaded")
	}
	chunks, err := s.chunks(ctx, f.ContentID)
	if err != nil {
		return err
	}
	if len(chunks) != f.ChunkCount {
		return fmt.Errorf("completed file has missing chunks")
	}
	onMetadata(f)
	for _, chunk := range chunks {
		plain, releaseChunk, err := s.readChunk(ctx, f.ContentID, chunk)
		if err != nil {
			return err
		}
		_, err = output.Write(plain)
		releaseChunk()
		if err != nil {
			return err
		}
	}
	return nil
}
