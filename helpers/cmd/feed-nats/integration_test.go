//go:build integration

// Integration test for the NATS feed bridge. Requires a running NATS reachable
// via $CODICIS_NATS_URL; skipped otherwise. Run with:
//
//	CODICIS_NATS_URL=nats://localhost:4222 \
//	  go test -tags integration ./cmd/feed-nats/
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestNATSBridge(t *testing.T) {
	url := os.Getenv("CODICIS_NATS_URL")
	if url == "" {
		t.Skip("set CODICIS_NATS_URL to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	nc, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer nc.Drain() //nolint:errcheck
	msgs := make(chan *nats.Msg, 4)
	if _, err := nc.ChanSubscribe("md.BTC", msgs); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

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

	sinkConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sink: %v", err)
	}
	sink := &natsSink{nc: sinkConn, prefix: "md."}
	defer sink.Close()
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 200 * time.Millisecond}, sink) //nolint:errcheck

	select {
	case m := <-msgs:
		if !strings.Contains(string(m.Data), "\"sym\":\"BTC\"") {
			t.Fatalf("payload: %q", string(m.Data))
		}
	case <-ctx.Done():
		t.Fatal("timed out waiting for the republished message")
	}
}
