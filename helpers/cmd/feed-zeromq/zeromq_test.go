// A self-contained test for the ZeroMQ bridge: it binds a real PUB socket and
// verifies a SUB client receives the republished feed line. No external broker
// (zmq4 is pure Go), so it runs in the normal suite.
package main

import (
	"context"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/go-zeromq/zmq4"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestZeroMQBridge(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	// PUB socket the bridge publishes on.
	pub := zmq4.NewPub(ctx)
	if err := pub.Listen("tcp://127.0.0.1:0"); err != nil {
		t.Fatalf("pub listen: %v", err)
	}
	defer pub.Close()
	endpoint := "tcp://" + pub.Addr().String()

	// SUB client filtering the symbol topic.
	sub := zmq4.NewSub(ctx)
	if err := sub.Dial(endpoint); err != nil {
		t.Fatalf("sub dial: %v", err)
	}
	defer sub.Close()
	if err := sub.SetOption(zmq4.OptionSubscribe, "md.BTC"); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	// A fake feed-helper that resends the line periodically (defeats ZeroMQ's
	// slow-joiner: the SUB subscription may register after the first send).
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
		tick := time.NewTicker(50 * time.Millisecond)
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

	sink := &zmqSink{pub: pub, prefix: "md."}
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 200 * time.Millisecond}, sink) //nolint:errcheck

	// Receive with a deadline via a goroutine + select.
	got := make(chan zmq4.Msg, 1)
	go func() {
		for {
			m, rerr := sub.Recv()
			if rerr != nil {
				return
			}
			got <- m
			return
		}
	}()

	select {
	case m := <-got:
		if len(m.Frames) < 2 || !strings.Contains(string(m.Frames[1]), "\"sym\":\"BTC\"") {
			t.Fatalf("frames: %q", m.Frames)
		}
	case <-ctx.Done():
		t.Fatal("timed out waiting for the SUB message")
	}
}
