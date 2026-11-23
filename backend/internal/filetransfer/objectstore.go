package filetransfer

import (
	"context"
	"fmt"
	"io"
	"strings"

	"github.com/google/uuid"
	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

// ObjectStore 是文件对象存储契约。Get 返回的流由调用者关闭，List 只允许规范的内容前缀，防止误扫或误删整个 Bucket。
type ObjectStore interface {
	Put(context.Context, string, io.Reader, int64) error
	Get(context.Context, string) (io.ReadCloser, error)
	List(context.Context, string) ([]string, error)
	Remove(context.Context, string) error
}
type MinIOStore struct {
	client *minio.Client
	bucket string
}

func NewMinIOStore(endpoint, accessKey, secretKey, bucket string, useSSL bool) (*MinIOStore, error) {
	client, err := minio.New(endpoint, &minio.Options{Creds: credentials.NewStaticV4(accessKey, secretKey, ""), Secure: useSSL})
	if err != nil {
		return nil, err
	}
	return &MinIOStore{client: client, bucket: bucket}, nil
}

// NewMinIOStoreFromClient adapts an existing client to the file object store.
func NewMinIOStoreFromClient(client *minio.Client, bucket string) *MinIOStore {
	return &MinIOStore{client: client, bucket: bucket}
}

func (s *MinIOStore) Ensure(ctx context.Context) error {
	exists, err := s.client.BucketExists(ctx, s.bucket)
	if err != nil {
		return err
	}
	if !exists {
		if err := s.client.MakeBucket(ctx, s.bucket, minio.MakeBucketOptions{}); err != nil {
			return err
		}
	}
	return nil
}
func (s *MinIOStore) Put(ctx context.Context, key string, reader io.Reader, size int64) error {
	_, err := s.client.PutObject(ctx, s.bucket, key, reader, size, minio.PutObjectOptions{ContentType: "application/octet-stream"})
	return err
}
func (s *MinIOStore) Get(ctx context.Context, key string) (io.ReadCloser, error) {
	object, err := s.client.GetObject(ctx, s.bucket, key, minio.GetObjectOptions{})
	if err != nil {
		return nil, err
	}
	if _, err := object.Stat(); err != nil {
		object.Close()
		return nil, fmt.Errorf("stat object: %w", err)
	}
	return object, nil
}
func (s *MinIOStore) List(ctx context.Context, prefix string) ([]string, error) {
	if !validContentPrefix(prefix) {
		return nil, fmt.Errorf("invalid object list prefix")
	}
	keys := make([]string, 0)
	for object := range s.client.ListObjects(ctx, s.bucket, minio.ListObjectsOptions{Prefix: prefix, Recursive: true}) {
		if object.Err != nil {
			return nil, object.Err
		}
		if !strings.HasPrefix(object.Key, prefix) {
			return nil, fmt.Errorf("object store returned key outside requested content prefix")
		}
		keys = append(keys, object.Key)
	}
	return keys, nil
}

func validContentPrefix(prefix string) bool {
	if !strings.HasPrefix(prefix, "contents/") || !strings.HasSuffix(prefix, "/") {
		return false
	}
	id := strings.TrimSuffix(strings.TrimPrefix(prefix, "contents/"), "/")
	parsed, err := uuid.Parse(id)
	return err == nil && parsed.String() == id
}

func (s *MinIOStore) Remove(ctx context.Context, key string) error {
	return s.client.RemoveObject(ctx, s.bucket, key, minio.RemoveObjectOptions{})
}
