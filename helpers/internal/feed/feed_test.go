package feed

import (
	"context"
	"net"
	"sync"
	"testing"
)

type recSink struct {
	mu sync.Mutex
	up []Update
}

func (r *recSink) Publish(_ context.Context, u Update) error {
	r.mu.Lock()
	r.up = append(r.up, u)
	r.mu.Unlock()
	return nil
}
func (r *recSink) Close() error { return nil }

func TestStreamRepublishes(t *testing.T) {
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
		// A greeting (no symbol) then two L1 updates, then close.
		c.Write([]byte("{\"t\":\"ready\"}\n"))
		c.Write([]byte("{\"t\":\"l1\",\"sym\":\"BTC\",\"bid\":101,\"ask\":105}\n"))
		c.Write([]byte("{\"t\":\"l1\",\"sym\":\"ETH\",\"bid\":50,\"ask\":51}\n"))
		c.Close()
	}()

	rec := &recSink{}
	// stream handles a single connection and returns when it closes.
	if err := stream(context.Background(),
		Options{Addr: ln.Addr().String()}, rec); err != nil {
		t.Fatalf("stream: %v", err)
	}

	// The ready greeting (no symbol) is filtered; the two symbol updates pass.
	if len(rec.up) != 2 {
		t.Fatalf("got %d updates: %+v", len(rec.up), rec.up)
	}
	if rec.up[0].Symbol != "BTC" || rec.up[0].Type != "l1" {
		t.Fatalf("first: %+v", rec.up[0])
	}
	if rec.up[1].Symbol != "ETH" {
		t.Fatalf("second: %+v", rec.up[1])
	}
}
