// Command feed-nats bridges the codicis market-data feed to NATS. It subscribes
// to the feed-helper's TCP stream and publishes each per-symbol update to the
// subject "<prefix><symbol>".
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR   feed-helper host:port
//	-nats / $CODICIS_NATS_URL    NATS URL (default nats://localhost:4222)
//	-prefix                      subject prefix (default "md.")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/nats-io/nats.go"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	natsURL := flag.String("nats", getenv("CODICIS_NATS_URL", nats.DefaultURL),
		"NATS URL")
	prefix := flag.String("prefix", "md.", "subject prefix")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-nats: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	nc, err := nats.Connect(*natsURL)
	if err != nil {
		log.Fatalf("feed-nats: connect: %v", err)
	}
	sink := &natsSink{nc: nc, prefix: *prefix}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-nats: %v", err)
	}
}

type natsSink struct {
	nc     *nats.Conn
	prefix string
}

func (s *natsSink) Publish(_ context.Context, u feed.Update) error {
	return s.nc.Publish(s.prefix+u.Symbol, u.Raw)
}
func (s *natsSink) Close() error {
	_ = s.nc.Drain()
	return nil
}

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
