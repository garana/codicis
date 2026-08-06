//go:build integration

// Requires a running RabbitMQ via $CODICIS_AMQP_URL; skipped otherwise.
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestRabbitBridge(t *testing.T) {
	url := os.Getenv("CODICIS_AMQP_URL")
	if url == "" {
		t.Skip("set CODICIS_AMQP_URL to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	conn, err := amqp.Dial(url)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	ch, err := conn.Channel()
	if err != nil {
		t.Fatalf("channel: %v", err)
	}
	const ex = "codicis.md.test"
	if err := ch.ExchangeDeclare(ex, "topic", true, false, false, false, nil); err != nil {
		t.Fatalf("exchange: %v", err)
	}
	q, err := ch.QueueDeclare("", false, true, true, false, nil)
	if err != nil {
		t.Fatalf("queue: %v", err)
	}
	if err := ch.QueueBind(q.Name, "md.BTC", ex, false, nil); err != nil {
		t.Fatalf("bind: %v", err)
	}
	deliveries, err := ch.Consume(q.Name, "", true, false, false, false, nil)
	if err != nil {
		t.Fatalf("consume: %v", err)
	}

	ln := fakeFeed(t, ctx)
	defer ln.Close()

	pubConn, _ := amqp.Dial(url)
	defer pubConn.Close()
	pubCh, _ := pubConn.Channel()
	sink := &amqpSink{ch: pubCh, exchange: ex, prefix: "md."}
	defer sink.Close()
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 200 * time.Millisecond}, sink) //nolint:errcheck

	select {
	case d := <-deliveries:
		if !strings.Contains(string(d.Body), "\"sym\":\"BTC\"") {
			t.Fatalf("body: %q", d.Body)
		}
	case <-ctx.Done():
		t.Fatal("timed out")
	}
}

// fakeFeed accepts one connection and resends an L1 line periodically.
func fakeFeed(t *testing.T, ctx context.Context) net.Listener {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	go func() {
		c, aerr := ln.Accept()
		if aerr != nil {
			return
		}
		defer c.Close()
		tick := time.NewTicker(100 * time.Millisecond)
		defer tick.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-tick.C:
				if _, werr := c.Write([]byte("{\"t\":\"l1\",\"sym\":\"BTC\",\"bid\":101}\n")); werr != nil {
					return
				}
			}
		}
	}()
	return ln
}
