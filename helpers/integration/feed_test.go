//go:build integration

// Black-box integration tests for the feed-bridge helper binaries. Each bridge
// SUBSCRIBES to the codicis feed-helper's newline-delimited JSON stream and
// re-PUBLISHES every per-symbol update to an external broker. These tests stand
// a small in-process TCP "feed source" in for the feed-helper (it repeats a
// fixed L1 stream, which also defeats broker slow-joiner / consumer lag), run
// the built bridge against a real broker (testcontainers, ephemeral port),
// subscribe to the broker, and snapshot the unique messages the bridge
// published ("<target>\t<payload>", deduplicated + sorted for determinism).
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

	amqp "github.com/rabbitmq/amqp091-go"
	"github.com/redis/go-redis/v9"
	"github.com/segmentio/kafka-go"
	tckafka "github.com/testcontainers/testcontainers-go/modules/kafka"
	tcnats "github.com/testcontainers/testcontainers-go/modules/nats"
	tcrabbitmq "github.com/testcontainers/testcontainers-go/modules/rabbitmq"
	tcredis "github.com/testcontainers/testcontainers-go/modules/redis"

	"github.com/nats-io/nats.go"
)

// feedLines is the fixed L1 stream the in-test feed source repeats; the bridge
// forwards each line verbatim to the broker keyed by its "sym". Three lines,
// two symbols -> three unique (target, payload) messages.
var feedLines = []string{
	`{"t":"l1","sym":"BTC","seq":1,"bid":101,"bid_qty":3,"ask":105,"ask_qty":8}`,
	`{"t":"l1","sym":"ETH","seq":2,"bid":200,"bid_qty":1,"ask":205,"ask_qty":2}`,
	`{"t":"l1","sym":"BTC","seq":3,"bid":102,"bid_qty":5,"ask":104,"ask_qty":6}`,
}

const feedUnique = 3

// startFeedSource listens on an ephemeral port and, to the bridge that connects,
// repeats the given lines until the connection drops (killed on cleanup). The
// repetition defeats brokers whose subscription registers slightly after the
// first publish (ZeroMQ slow-joiner, consumer-group lag).
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
				for {
					for _, l := range lines {
						if _, err := io.WriteString(c, l+"\n"); err != nil {
							return
						}
					}
					time.Sleep(100 * time.Millisecond)
				}
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

// collect gathers unique message keys via recv until it has `feedUnique`
// distinct ones or times out, then renders a sorted snapshot. recv returns one
// key and whether it produced one (false on its own short timeout).
func collect(t *testing.T, recv func() (string, bool)) []byte {
	t.Helper()
	set := map[string]struct{}{}
	deadline := time.Now().Add(60 * time.Second)
	for len(set) < feedUnique {
		if time.Now().After(deadline) {
			t.Fatalf("collected %d/%d unique messages: %v", len(set), feedUnique, sortedKeys(set))
		}
		if k, ok := recv(); ok {
			set[k] = struct{}{}
		}
	}
	return []byte(strings.Join(sortedKeys(set), "\n") + "\n")
}

// waitPort blocks until addr accepts a TCP connection (a spawned bridge has
// bound its listener) or fails the test.
func waitPort(t *testing.T, addr string) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if c, err := net.DialTimeout("tcp", addr, time.Second); err == nil {
			_ = c.Close()
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("port %s never came up", addr)
}

func sortedKeys(set map[string]struct{}) []string {
	out := make([]string, 0, len(set))
	for k := range set {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func TestFeedRedis(t *testing.T) {
	ctx := context.Background()
	c, err := tcredis.Run(ctx, "redis:7-alpine")
	if err != nil {
		t.Fatalf("start redis: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	uri, _ := c.ConnectionString(ctx)
	addr := strings.TrimPrefix(uri, "redis://")

	rdb := redis.NewClient(&redis.Options{Addr: addr})
	defer rdb.Close()
	sub := rdb.PSubscribe(ctx, "md.*")
	defer sub.Close()
	if _, err := sub.Receive(ctx); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	ch := sub.Channel()

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-redis", "-feed", src, "-redis", addr, "-prefix", "md.")

	got := collect(t, func() (string, bool) {
		select {
		case m := <-ch:
			return fmt.Sprintf("%s\t%s", m.Channel, m.Payload), true
		case <-time.After(500 * time.Millisecond):
			return "", false
		}
	})
	snapshot(t, "feed-redis", got)
}

func TestFeedNATS(t *testing.T) {
	ctx := context.Background()
	c, err := tcnats.Run(ctx, "nats:2")
	if err != nil {
		t.Fatalf("start nats: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	uri, _ := c.ConnectionString(ctx)

	nc, err := nats.Connect(uri)
	if err != nil {
		t.Fatalf("nats connect: %v", err)
	}
	defer nc.Close()
	msgCh := make(chan *nats.Msg, 64)
	if _, err := nc.ChanSubscribe("md.*", msgCh); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	_ = nc.Flush()

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-nats", "-feed", src, "-nats", uri, "-prefix", "md.")

	got := collect(t, func() (string, bool) {
		select {
		case m := <-msgCh:
			return fmt.Sprintf("%s\t%s", m.Subject, string(m.Data)), true
		case <-time.After(500 * time.Millisecond):
			return "", false
		}
	})
	snapshot(t, "feed-nats", got)
}

func TestFeedKafka(t *testing.T) {
	ctx := context.Background()
	c, err := tckafka.Run(ctx, "confluentinc/confluent-local:7.6.1")
	if err != nil {
		t.Fatalf("start kafka: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	brokers, err := c.Brokers(ctx)
	if err != nil {
		t.Fatalf("brokers: %v", err)
	}

	// Pre-create the per-symbol topics so the reader can start cleanly.
	for _, topic := range []string{"md.BTC", "md.ETH"} {
		conn, err := kafka.DialLeader(ctx, "tcp", brokers[0], topic, 0)
		if err != nil {
			t.Fatalf("create topic %s: %v", topic, err)
		}
		_ = conn.Close()
	}
	r := kafka.NewReader(kafka.ReaderConfig{
		Brokers:     brokers,
		GroupTopics: []string{"md.BTC", "md.ETH"},
		GroupID:     "codicis-it",
		StartOffset: kafka.FirstOffset,
	})
	defer r.Close()

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-kafka", "-feed", src, "-brokers", strings.Join(brokers, ","), "-prefix", "md.")

	got := collect(t, func() (string, bool) {
		rctx, cancel := context.WithTimeout(ctx, 2*time.Second)
		defer cancel()
		m, err := r.ReadMessage(rctx)
		if err != nil {
			return "", false
		}
		return fmt.Sprintf("%s\t%s", m.Topic, m.Value), true
	})
	snapshot(t, "feed-kafka", got)
}

func TestFeedRabbitMQ(t *testing.T) {
	ctx := context.Background()
	c, err := tcrabbitmq.Run(ctx, "rabbitmq:3-alpine")
	if err != nil {
		t.Fatalf("start rabbitmq: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	url, err := c.AmqpURL(ctx)
	if err != nil {
		t.Fatalf("amqp url: %v", err)
	}

	const exchange = "codicis.md"
	conn, err := amqp.Dial(url)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	ch, err := conn.Channel()
	if err != nil {
		t.Fatalf("channel: %v", err)
	}
	if err := ch.ExchangeDeclare(exchange, "topic", true, false, false, false, nil); err != nil {
		t.Fatalf("exchange: %v", err)
	}
	q, err := ch.QueueDeclare("", false, true, true, false, nil)
	if err != nil {
		t.Fatalf("queue: %v", err)
	}
	if err := ch.QueueBind(q.Name, "md.#", exchange, false, nil); err != nil {
		t.Fatalf("bind: %v", err)
	}
	deliveries, err := ch.Consume(q.Name, "", true, false, false, false, nil)
	if err != nil {
		t.Fatalf("consume: %v", err)
	}

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-rabbitmq", "-feed", src, "-amqp", url,
		"-exchange", exchange, "-prefix", "md.")

	got := collect(t, func() (string, bool) {
		select {
		case d := <-deliveries:
			return fmt.Sprintf("%s\t%s", d.RoutingKey, d.Body), true
		case <-time.After(500 * time.Millisecond):
			return "", false
		}
	})
	snapshot(t, "feed-rabbitmq", got)
}
