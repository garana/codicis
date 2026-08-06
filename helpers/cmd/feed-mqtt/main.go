// Command feed-mqtt bridges the codicis market-data feed to an MQTT broker. It
// subscribes to the feed-helper's TCP stream and publishes each per-symbol
// update to topic "<prefix><symbol>".
//
// Uses eclipse/paho.mqtt.golang (dual-licensed EPL-2.0 / EDL-1.0; used here
// under the permissive EDL-1.0).
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR   feed-helper host:port
//	-broker / $CODICIS_MQTT_URL  broker URL (default tcp://localhost:1883)
//	-prefix                      topic prefix (default "md/")
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	broker := flag.String("broker", getenv("CODICIS_MQTT_URL", "tcp://localhost:1883"),
		"MQTT broker URL")
	prefix := flag.String("prefix", "md/", "topic prefix")
	qos := flag.Int("qos", 0, "MQTT QoS (0,1,2)")
	flag.Parse()
	if *feedAddr == "" {
		log.Fatal("feed-mqtt: -feed or $CODICIS_FEED_ADDR is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	opts := mqtt.NewClientOptions().AddBroker(*broker).
		SetClientID("codicis-feed-mqtt").SetConnectTimeout(5 * time.Second)
	client := mqtt.NewClient(opts)
	if tok := client.Connect(); tok.Wait() && tok.Error() != nil {
		log.Fatalf("feed-mqtt: connect: %v", tok.Error())
	}
	sink := &mqttSink{client: client, prefix: *prefix, qos: byte(*qos)}
	defer sink.Close()
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-mqtt: %v", err)
	}
}

type mqttSink struct {
	client mqtt.Client
	prefix string
	qos    byte
}

func (s *mqttSink) Publish(_ context.Context, u feed.Update) error {
	tok := s.client.Publish(s.prefix+u.Symbol, s.qos, false, u.Raw)
	tok.Wait()
	return tok.Error()
}
func (s *mqttSink) Close() error {
	s.client.Disconnect(250)
	return nil
}

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
