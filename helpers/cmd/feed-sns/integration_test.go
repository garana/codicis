//go:build integration

// Requires an SNS+SQS endpoint (e.g. LocalStack) via $AWS_ENDPOINT_URL; skipped
// otherwise. The bridge publishes to an SNS topic; the test subscribes an SQS
// queue to it (raw delivery) and asserts the message arrives.
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
	"github.com/aws/aws-sdk-go-v2/service/sns"
	"github.com/aws/aws-sdk-go-v2/service/sqs"
	sqstypes "github.com/aws/aws-sdk-go-v2/service/sqs/types"

	"github.com/garana/codicis/helpers/internal/feed"
)

func TestSNSBridge(t *testing.T) {
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
	snsc := sns.NewFromConfig(cfg, func(o *sns.Options) { o.BaseEndpoint = aws.String(endpoint) })
	sqsc := sqs.NewFromConfig(cfg, func(o *sqs.Options) { o.BaseEndpoint = aws.String(endpoint) })

	topic, err := snsc.CreateTopic(ctx, &sns.CreateTopicInput{Name: aws.String("codicis-md-sns-it")})
	if err != nil {
		t.Fatalf("create topic: %v", err)
	}
	q, err := sqsc.CreateQueue(ctx, &sqs.CreateQueueInput{QueueName: aws.String("codicis-md-sns-q")})
	if err != nil {
		t.Fatalf("create queue: %v", err)
	}
	attr, err := sqsc.GetQueueAttributes(ctx, &sqs.GetQueueAttributesInput{
		QueueUrl:       q.QueueUrl,
		AttributeNames: []sqstypes.QueueAttributeName{sqstypes.QueueAttributeNameQueueArn}})
	if err != nil {
		t.Fatalf("queue attrs: %v", err)
	}
	queueArn := attr.Attributes["QueueArn"]
	if _, err := snsc.Subscribe(ctx, &sns.SubscribeInput{
		TopicArn: topic.TopicArn, Protocol: aws.String("sqs"),
		Endpoint: aws.String(queueArn), ReturnSubscriptionArn: true,
		Attributes: map[string]string{"RawMessageDelivery": "true"}}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	ln := fakeFeedSNS(t, ctx)
	defer ln.Close()

	sink := &snsSink{client: snsc, topic: *topic.TopicArn}
	go feed.Run(ctx, feed.Options{Addr: ln.Addr().String(), Reconnect: 300 * time.Millisecond}, sink) //nolint:errcheck

	deadline := time.After(20 * time.Second)
	for {
		select {
		case <-deadline:
			t.Fatal("timed out waiting for the SNS->SQS message")
		default:
		}
		out, rerr := sqsc.ReceiveMessage(ctx, &sqs.ReceiveMessageInput{
			QueueUrl: q.QueueUrl, MaxNumberOfMessages: 1, WaitTimeSeconds: 2})
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

func fakeFeedSNS(t *testing.T, ctx context.Context) net.Listener {
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
