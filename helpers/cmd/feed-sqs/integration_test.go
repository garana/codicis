//go:build integration

// Requires an SQS endpoint (e.g. LocalStack) via $AWS_ENDPOINT_URL; skipped
// otherwise. Also expects AWS_REGION and AWS credentials in the environment
// (LocalStack accepts any).
package main

import (
	"context"
	"net"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/sqs"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestSQSBridge(t *testing.T) {
	endpoint := os.Getenv("AWS_ENDPOINT_URL")
	if endpoint == "" {
		t.Skip("set AWS_ENDPOINT_URL (LocalStack) to run")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	cfg, err := config.LoadDefaultConfig(ctx, config.WithRegion(region()))
	if err != nil {
		t.Fatalf("aws config: %v", err)
	}
	client := sqs.NewFromConfig(cfg, func(o *sqs.Options) {
		o.BaseEndpoint = aws.String(endpoint)
	})
	created, err := client.CreateQueue(ctx, &sqs.CreateQueueInput{
		QueueName: aws.String("codicis-md-it")})
	if err != nil {
		t.Fatalf("create queue: %v", err)
	}
	queueURL := *created.QueueUrl

	ln := fakeFeedSQS(t, ctx)
	defer ln.Close()

	sink := &sqsSink{client: client, queue: queueURL}
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 300 * time.Millisecond}, sink) //nolint:errcheck

	deadline := time.After(20 * time.Second)
	for {
		select {
		case <-deadline:
			t.Fatal("timed out waiting for an SQS message")
		default:
		}
		out, rerr := client.ReceiveMessage(ctx, &sqs.ReceiveMessageInput{
			QueueUrl: aws.String(queueURL), MaxNumberOfMessages: 1,
			WaitTimeSeconds: 2})
		if rerr != nil {
			t.Fatalf("receive: %v", rerr)
		}
		if len(out.Messages) > 0 {
			if !strings.Contains(*out.Messages[0].Body, "\"sym\":\"BTC\"") {
				t.Fatalf("body: %q", *out.Messages[0].Body)
			}
			return
		}
	}
}

func region() string {
	if r := os.Getenv("AWS_REGION"); r != "" {
		return r
	}
	return "us-east-1"
}

func fakeFeedSQS(t *testing.T, ctx context.Context) net.Listener {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	go func() {
		c, aerr := ln.Accept()
		if aerr != nil {
			return
		}
		defer c.Close()
		tick := time.NewTicker(300 * time.Millisecond)
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
	return ln
}
