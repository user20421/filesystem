FROM golang:1.27-alpine AS build
WORKDIR /src
ARG GOPROXY=https://goproxy.cn,direct
ENV GOPROXY=${GOPROXY}
COPY go.mod go.sum ./
RUN go mod download
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/server ./cmd/server

FROM alpine:3.22
RUN apk add --no-cache ca-certificates wget \
    && addgroup -S secure-im \
    && adduser -S -G secure-im secure-im
COPY --from=build /out/server /usr/local/bin/secure-im-server
USER secure-im
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/secure-im-server"]
