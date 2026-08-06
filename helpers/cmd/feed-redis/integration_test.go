//go:build integration

// Integration test for the Redis feed bridge. Requires a running Redis reachable
// via $CODICIS_REDIS_ADDR; skipped otherwise. Run with:
//
//	CODICIS_REDIS_ADDR=localhost:6379 \
//	  go test -tags integration ./cmd/feed-redis/
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestRedisBridge(t *testing.T) {
	addr := os.Getenv("CODICIS_REDIS_ADDR")
	if addr == "" {
		t.Skip("set CODICIS_REDIS_ADDR to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Subscribe to the channel the bridge will publish to.
	rdb := redis.NewClient(&redis.Options{Addr: addr})
	defer rdb.Close()
	sub := rdb.Subscribe(ctx, "md.BTC")
	defer sub.Close()
	if _, err := sub.Receive(ctx); err != nil { // wait until subscribed
		t.Fatalf("subscribe: %v", err)
	}
	ch := sub.Channel()

	// A fake feed-helper: send one L1 line, then keep the connection open.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	go func() {
		c, aerr := ln.Accept()
		if aerr != nil {
			return
		}
		c.Write([]byte("{\"t\":\"l1\",\"sym\":\"BTC\",\"bid\":101,\"ask\":105}\n"))
		<-ctx.Done()
		c.Close()
	}()

	sink := &redisSink{rdb: redis.NewClient(&redis.Options{Addr: addr}), prefix: "md."}
	defer sink.Close()
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 200 * time.Millisecond}, sink) //nolint:errcheck

	select {
	case msg := <-ch:
		if !strings.Contains(msg.Payload, "\"sym\":\"BTC\"") {
			t.Fatalf("payload: %q", msg.Payload)
		}
	case <-ctx.Done():
		t.Fatal("timed out waiting for the republished message")
	}
}
