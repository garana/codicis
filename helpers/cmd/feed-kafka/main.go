// Command feed-kafka bridges the codicis market-data feed to Apache Kafka. It
// subscribes to the feed-helper's TCP stream and produces each per-symbol
// update to topic "<prefix><symbol>" (keyed by symbol for partition affinity).
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR      feed-helper host:port
//	-brokers / $CODICIS_KAFKA_BROKERS  comma-separated brokers
//	-prefix                         topic prefix (default "md.")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/segmentio/kafka-go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	brokers := flag.String("brokers", getenv("CODICIS_KAFKA_BROKERS", "localhost:9092"),
		"comma-separated Kafka brokers")
	prefix := flag.String("prefix", "md.", "topic prefix")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-kafka: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	w := &kafka.Writer{
		Addr:                   kafka.TCP(strings.Split(*brokers, ",")...),
		Balancer:               &kafka.Hash{},
		AllowAutoTopicCreation: true,
	}
	sink := &kafkaSink{w: w, prefix: *prefix}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-kafka: %v", err)
	}
}

type kafkaSink struct {
	w      *kafka.Writer
	prefix string
}

func (s *kafkaSink) Publish(ctx context.Context, u feed.Update) error {
	return s.w.WriteMessages(ctx, kafka.Message{
		Topic: s.prefix + u.Symbol,
		Key:   []byte(u.Symbol),
		Value: u.Raw,
	})
}
func (s *kafkaSink) Close() error { return s.w.Close() }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
