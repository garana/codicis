//go:build integration

// Black-box integration tests for the feed-bridge helper binaries. Each bridge
// SUBSCRIBES to the codicis feed-helper's newline-delimited JSON stream and
// re-PUBLISHES every per-symbol update to an external broker. These tests stand
// in a small in-process TCP "feed source" for the feed-helper, run the built
// bridge against a real broker (testcontainers, ephemeral port), subscribe to
// the broker to capture what the bridge published, and snapshot it.
package integration

import (
	"context"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	tcnats "github.com/testcontainers/testcontainers-go/modules/nats"
	tcredis "github.com/testcontainers/testcontainers-go/modules/redis"

	"github.com/nats-io/nats.go"
)

// feedLines is the fixed L1 stream the in-test feed source emits; the bridge
// forwards each line verbatim to the broker keyed by its "sym".
var feedLines = []string{
	`{"t":"l1","sym":"BTC","seq":1,"bid":101,"bid_qty":3,"ask":105,"ask_qty":8}`,
	`{"t":"l1","sym":"ETH","seq":2,"bid":200,"bid_qty":1,"ask":205,"ask_qty":2}`,
	`{"t":"l1","sym":"BTC","seq":3,"bid":102,"bid_qty":5,"ask":104,"ask_qty":6}`,
}

// startFeedSource listens on an ephemeral port and, to each client that
// connects (the bridge), writes the given lines then holds the connection open
// briefly so the bridge stays subscribed. Returns host:port.
func startFeedSource(t *testing.T, lines []string) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("feed source listen: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				for _, l := range lines {
					if _, err := io.WriteString(c, l+"\n"); err != nil {
						return
					}
				}
				// Hold the connection open (no reconnect/duplicate sends) until
				// the bridge is killed on cleanup.
				_, _ = io.Copy(io.Discard, c)
			}(conn)
		}
	}()
	return ln.Addr().String()
}

// runBridge starts a feed-bridge binary and reaps it on cleanup.
func runBridge(t *testing.T, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(helperBin(t, name), args...)
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start %s: %v", name, err)
	}
	t.Cleanup(func() { _ = cmd.Process.Kill(); _ = cmd.Wait() })
}

// renderMessages formats captured "<target>\t<payload>" lines into a stable
// snapshot: sorted, so broker delivery-order jitter does not cause flakes.
func renderMessages(msgs []string) []byte {
	sort.Strings(msgs)
	return []byte(strings.Join(msgs, "\n") + "\n")
}

func TestFeedRedis(t *testing.T) {
	ctx := context.Background()
	c, err := tcredis.Run(ctx, "redis:7-alpine")
	if err != nil {
		t.Fatalf("start redis: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	uri, err := c.ConnectionString(ctx) // redis://host:port
	if err != nil {
		t.Fatalf("uri: %v", err)
	}
	addr := strings.TrimPrefix(uri, "redis://")

	rdb := redis.NewClient(&redis.Options{Addr: addr})
	defer rdb.Close()
	sub := rdb.PSubscribe(ctx, "md.*")
	defer sub.Close()
	if _, err := sub.Receive(ctx); err != nil { // wait for the subscription to be ready
		t.Fatalf("subscribe: %v", err)
	}
	ch := sub.Channel()

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-redis", "-feed", src, "-redis", addr, "-prefix", "md.")

	var msgs []string
	timeout := time.After(30 * time.Second)
	for len(msgs) < len(feedLines) {
		select {
		case m := <-ch:
			msgs = append(msgs, fmt.Sprintf("%s\t%s", m.Channel, m.Payload))
		case <-timeout:
			t.Fatalf("got %d/%d messages: %v", len(msgs), len(feedLines), msgs)
		}
	}
	snapshot(t, "feed-redis", renderMessages(msgs))
}

func TestFeedNATS(t *testing.T) {
	ctx := context.Background()
	c, err := tcnats.Run(ctx, "nats:2")
	if err != nil {
		t.Fatalf("start nats: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	uri, err := c.ConnectionString(ctx) // nats://host:port
	if err != nil {
		t.Fatalf("uri: %v", err)
	}

	nc, err := nats.Connect(uri)
	if err != nil {
		t.Fatalf("nats connect: %v", err)
	}
	defer nc.Close()
	msgCh := make(chan *nats.Msg, 16)
	if _, err := nc.ChanSubscribe("md.*", msgCh); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if err := nc.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-nats", "-feed", src, "-nats", uri, "-prefix", "md.")

	var msgs []string
	timeout := time.After(30 * time.Second)
	for len(msgs) < len(feedLines) {
		select {
		case m := <-msgCh:
			msgs = append(msgs, fmt.Sprintf("%s\t%s", m.Subject, string(m.Data)))
		case <-timeout:
			t.Fatalf("got %d/%d messages: %v", len(msgs), len(feedLines), msgs)
		}
	}
	snapshot(t, "feed-nats", renderMessages(msgs))
}
