//go:build integration

// Requires a running MQTT broker via $CODICIS_MQTT_URL; skipped otherwise.
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestMQTTBridge(t *testing.T) {
	url := os.Getenv("CODICIS_MQTT_URL")
	if url == "" {
		t.Skip("set CODICIS_MQTT_URL to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	got := make(chan []byte, 1)
	sopts := mqtt.NewClientOptions().AddBroker(url).SetClientID("codicis-mqtt-it-sub")
	sc := mqtt.NewClient(sopts)
	if tok := sc.Connect(); tok.Wait() && tok.Error() != nil {
		t.Fatalf("sub connect: %v", tok.Error())
	}
	defer sc.Disconnect(200)
	if tok := sc.Subscribe("md/BTC", 0, func(_ mqtt.Client, m mqtt.Message) {
		select {
		case got <- m.Payload():
		default:
		}
	}); tok.Wait() && tok.Error() != nil {
		t.Fatalf("subscribe: %v", tok.Error())
	}

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

	popts := mqtt.NewClientOptions().AddBroker(url).SetClientID("codicis-mqtt-it-pub")
	pc := mqtt.NewClient(popts)
	if tok := pc.Connect(); tok.Wait() && tok.Error() != nil {
		t.Fatalf("pub connect: %v", tok.Error())
	}
	sink := &mqttSink{client: pc, prefix: "md/", qos: 0}
	defer sink.Close()
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 200 * time.Millisecond}, sink) //nolint:errcheck

	select {
	case p := <-got:
		if !strings.Contains(string(p), "\"sym\":\"BTC\"") {
			t.Fatalf("payload: %q", p)
		}
	case <-ctx.Done():
		t.Fatal("timed out")
	}
}
