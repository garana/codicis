//go:build integration

package integration

import (
	"context"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/go-zeromq/zmq4"
	"github.com/testcontainers/testcontainers-go"
	"github.com/testcontainers/testcontainers-go/wait"
)

func TestFeedMQTT(t *testing.T) {
	ctx := context.Background()
	// No official mosquitto module: a generic container with an anonymous,
	// single-listener config.
	req := testcontainers.ContainerRequest{
		Image:        "eclipse-mosquitto:2",
		ExposedPorts: []string{"1883/tcp"},
		Files: []testcontainers.ContainerFile{{
			Reader:            strings.NewReader("listener 1883\nallow_anonymous true\n"),
			ContainerFilePath: "/mosquitto/config/mosquitto.conf",
			FileMode:          0o644,
		}},
		WaitingFor: wait.ForListeningPort("1883/tcp"),
	}
	c, err := testcontainers.GenericContainer(ctx, testcontainers.GenericContainerRequest{
		ContainerRequest: req, Started: true,
	})
	if err != nil {
		t.Fatalf("start mosquitto: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	host, _ := c.Host(ctx)
	port, _ := c.MappedPort(ctx, "1883/tcp")
	url := fmt.Sprintf("tcp://%s:%s", host, port.Port())

	msgCh := make(chan string, 64)
	sopts := mqtt.NewClientOptions().AddBroker(url).SetClientID("codicis-it-sub")
	sc := mqtt.NewClient(sopts)
	if tok := sc.Connect(); tok.Wait() && tok.Error() != nil {
		t.Fatalf("sub connect: %v", tok.Error())
	}
	defer sc.Disconnect(200)
	if tok := sc.Subscribe("md/#", 0, func(_ mqtt.Client, m mqtt.Message) {
		select {
		case msgCh <- fmt.Sprintf("%s\t%s", m.Topic(), m.Payload()):
		default:
		}
	}); tok.Wait() && tok.Error() != nil {
		t.Fatalf("subscribe: %v", tok.Error())
	}

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-mqtt", "-feed", src, "-broker", url, "-prefix", "md/")

	got := collect(t, func() (string, bool) {
		select {
		case k := <-msgCh:
			return k, true
		case <-time.After(500 * time.Millisecond):
			return "", false
		}
	})
	snapshot(t, "feed-mqtt", got)
}

func TestFeedZeroMQ(t *testing.T) {
	ctx := context.Background()

	// Pick a free port for the bridge's PUB socket to bind.
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("pick port: %v", err)
	}
	bindPort := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()
	endpoint := fmt.Sprintf("tcp://127.0.0.1:%d", bindPort)

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-zeromq", "-feed", src, "-bind", endpoint, "-prefix", "md.")
	waitPort(t, fmt.Sprintf("127.0.0.1:%d", bindPort)) // bridge binds its PUB first

	sub := zmq4.NewSub(ctx)
	defer sub.Close()
	if err := sub.Dial(endpoint); err != nil {
		t.Fatalf("sub dial: %v", err)
	}
	if err := sub.SetOption(zmq4.OptionSubscribe, "md."); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	// Recv blocks; funnel it through a channel so collect can time-box.
	msgCh := make(chan string, 64)
	go func() {
		for {
			m, err := sub.Recv()
			if err != nil {
				return
			}
			if len(m.Frames) >= 2 {
				msgCh <- fmt.Sprintf("%s\t%s", m.Frames[0], m.Frames[1])
			}
		}
	}()

	got := collect(t, func() (string, bool) {
		select {
		case k := <-msgCh:
			return k, true
		case <-time.After(500 * time.Millisecond):
			return "", false
		}
	})
	snapshot(t, "feed-zeromq", got)
}
