// Command feed-redis bridges the codicis market-data feed to Redis pub/sub. It
// subscribes to the feed-helper's TCP stream and PUBLISHes each per-symbol
// update to a channel "<prefix><symbol>".
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR    feed-helper host:port
//	-redis / $CODICIS_REDIS_ADDR  redis host:port (default localhost:6379)
//	-prefix                       channel prefix (default "md.")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/redis/go-redis/v9"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	redisAddr := flag.String("redis", getenv("CODICIS_REDIS_ADDR", "localhost:6379"),
		"redis host:port")
	prefix := flag.String("prefix", "md.", "channel prefix")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-redis: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	sink := &redisSink{
		rdb:    redis.NewClient(&redis.Options{Addr: *redisAddr}),
		prefix: *prefix,
	}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-redis: %v", err)
	}
}

type redisSink struct {
	rdb    *redis.Client
	prefix string
}

func (s *redisSink) Publish(ctx context.Context, u feed.Update) error {
	return s.rdb.Publish(ctx, s.prefix+u.Symbol, u.Raw).Err()
}
func (s *redisSink) Close() error { return s.rdb.Close() }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
