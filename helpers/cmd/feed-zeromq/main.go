// Command feed-zeromq bridges the codicis market-data feed to ZeroMQ. It
// subscribes to the feed-helper's TCP stream and publishes each update over a
// PUB socket as a two-frame message: [ "<prefix><symbol>" topic, JSON payload ].
// SUB clients filter by the symbol-prefixed topic frame.
//
// Uses go-zeromq/zmq4 -- a pure-Go implementation (no libzmq, no CGO), BSD-3.
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR    feed-helper host:port
//	-bind / $CODICIS_ZMQ_BIND     PUB endpoint to bind (default tcp://*:5560)
//	-prefix                       topic prefix (default "md.")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/go-zeromq/zmq4"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	bind := flag.String("bind", getenv("CODICIS_ZMQ_BIND", "tcp://*:5560"),
		"PUB endpoint to bind")
	prefix := flag.String("prefix", "md.", "topic prefix")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-zeromq: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	pub := zmq4.NewPub(ctx)
	if err := pub.Listen(*bind); err != nil {
		log.Fatalf("feed-zeromq: bind %s: %v", *bind, err)
	}
	sink := &zmqSink{pub: pub, prefix: *prefix}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-zeromq: %v", err)
	}
}

type zmqSink struct {
	pub    zmq4.Socket
	prefix string
}

func (s *zmqSink) Publish(_ context.Context, u feed.Update) error {
	// Topic frame first so SUB sockets can prefix-filter by symbol.
	return s.pub.Send(zmq4.NewMsgFrom([]byte(s.prefix+u.Symbol), u.Raw))
}
func (s *zmqSink) Close() error { return s.pub.Close() }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
