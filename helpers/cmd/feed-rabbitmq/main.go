// Command feed-rabbitmq bridges the codicis market-data feed to RabbitMQ
// (AMQP 0-9-1). It subscribes to the feed-helper's TCP stream and publishes each
// per-symbol update to a topic exchange with routing key "<prefix><symbol>".
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR     feed-helper host:port
//	-amqp / $CODICIS_AMQP_URL      AMQP URL (default amqp://guest:guest@localhost:5672/)
//	-exchange                      topic exchange name (default "codicis.md")
//	-prefix                        routing-key prefix (default "md.")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	amqp "github.com/rabbitmq/amqp091-go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	amqpURL := flag.String("amqp", getenv("CODICIS_AMQP_URL", "amqp://guest:guest@localhost:5672/"),
		"AMQP URL")
	exchange := flag.String("exchange", "codicis.md", "topic exchange name")
	prefix := flag.String("prefix", "md.", "routing-key prefix")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-rabbitmq: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	conn, err := amqp.Dial(*amqpURL)
	if err != nil {
		log.Fatalf("feed-rabbitmq: dial: %v", err)
	}
	defer conn.Close()
	ch, err := conn.Channel()
	if err != nil {
		log.Fatalf("feed-rabbitmq: channel: %v", err)
	}
	if err := ch.ExchangeDeclare(*exchange, "topic", true, false, false, false, nil); err != nil {
		log.Fatalf("feed-rabbitmq: exchange: %v", err)
	}

	sink := &amqpSink{ch: ch, exchange: *exchange, prefix: *prefix}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-rabbitmq: %v", err)
	}
}

type amqpSink struct {
	ch       *amqp.Channel
	exchange string
	prefix   string
}

func (s *amqpSink) Publish(ctx context.Context, u feed.Update) error {
	return s.ch.PublishWithContext(ctx, s.exchange, s.prefix+u.Symbol,
		false, false, amqp.Publishing{ContentType: "application/json", Body: u.Raw})
}
func (s *amqpSink) Close() error { return s.ch.Close() }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
