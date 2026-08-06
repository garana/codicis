// Command feed-sqs bridges the codicis market-data feed to an AWS SQS queue. It
// subscribes to the feed-helper's TCP stream and SendMessages each update to
// the queue, with the symbol as a "symbol" message attribute.
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR   feed-helper host:port
//	-queue / $CODICIS_SQS_URL    SQS queue URL
//	-region                      AWS region (default us-east-1)
//	-endpoint                    override endpoint (e.g. LocalStack)
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/sqs"
	"github.com/aws/aws-sdk-go-v2/service/sqs/types"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	queue := flag.String("queue", os.Getenv("CODICIS_SQS_URL"), "SQS queue URL")
	region := flag.String("region", getenv("AWS_REGION", "us-east-1"), "AWS region")
	endpoint := flag.String("endpoint", os.Getenv("AWS_ENDPOINT_URL"),
		"override endpoint (LocalStack)")
	flag.Parse()
	if *feedAddr == "" || *queue == "" {
		log.Fatal("feed-sqs: -feed and -queue are required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	cfg, err := config.LoadDefaultConfig(ctx, config.WithRegion(*region))
	if err != nil {
		log.Fatalf("feed-sqs: aws config: %v", err)
	}
	client := sqs.NewFromConfig(cfg, func(o *sqs.Options) {
		if *endpoint != "" {
			o.BaseEndpoint = aws.String(*endpoint)
		}
	})
	sink := &sqsSink{client: client, queue: *queue}
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-sqs: %v", err)
	}
}

type sqsSink struct {
	client *sqs.Client
	queue  string
}

func (s *sqsSink) Publish(ctx context.Context, u feed.Update) error {
	_, err := s.client.SendMessage(ctx, &sqs.SendMessageInput{
		QueueUrl:    aws.String(s.queue),
		MessageBody: aws.String(string(u.Raw)),
		MessageAttributes: map[string]types.MessageAttributeValue{
			"symbol": {DataType: aws.String("String"),
				StringValue: aws.String(u.Symbol)},
		},
	})
	return err
}
func (s *sqsSink) Close() error { return nil }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
