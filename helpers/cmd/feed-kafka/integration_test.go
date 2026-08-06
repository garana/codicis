//go:build integration

// Requires a running Kafka via $CODICIS_KAFKA_BROKERS; skipped otherwise.
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/segmentio/kafka-go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestKafkaBridge(t *testing.T) {
	brokers := os.Getenv("CODICIS_KAFKA_BROKERS")
	if brokers == "" {
		t.Skip("set CODICIS_KAFKA_BROKERS to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	const topic = "md.BTC"
	// Ensure the topic exists so the reader can start cleanly.
	conn, err := kafka.DialLeader(ctx, "tcp", strings.Split(brokers, ",")[0], topic, 0)
	if err != nil {
		t.Fatalf("dial leader (create topic): %v", err)
	}
	conn.Close()

	r := kafka.NewReader(kafka.ReaderConfig{
		Brokers: strings.Split(brokers, ","),
		Topic:   topic,
		GroupID: "codicis-kafka-it",
	})
	defer r.Close()

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
		defer c.Close()
		tick := time.NewTicker(200 * time.Millisecond)
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

	w := &kafka.Writer{Addr: kafka.TCP(strings.Split(brokers, ",")...),
		Balancer: &kafka.Hash{}, AllowAutoTopicCreation: true}
	sink := &kafkaSink{w: w, prefix: "md."}
	defer sink.Close()
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 300 * time.Millisecond}, sink) //nolint:errcheck

	m, err := r.ReadMessage(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if !strings.Contains(string(m.Value), "\"sym\":\"BTC\"") {
		t.Fatalf("value: %q", m.Value)
	}
}
