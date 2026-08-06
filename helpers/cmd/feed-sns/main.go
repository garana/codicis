// Command feed-sns bridges the codicis market-data feed to an AWS SNS topic. It
// subscribes to the feed-helper's TCP stream and Publishes each update to the
// topic, with the symbol as a "symbol" message attribute (so subscribers can
// filter by symbol).
//
// Config (flags override env):
//
//	-feed / $CODICIS_FEED_ADDR   feed-helper host:port
//	-topic / $CODICIS_SNS_TOPIC  SNS topic ARN
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
	"github.com/aws/aws-sdk-go-v2/service/sns"
	"github.com/aws/aws-sdk-go-v2/service/sns/types"

	"github.com/garana/codicis/helpers/internal/feed"
)

func main() {
	feedAddr := flag.String("feed", os.Getenv("CODICIS_FEED_ADDR"),
		"feed-helper host:port")
	topic := flag.String("topic", os.Getenv("CODICIS_SNS_TOPIC"), "SNS topic ARN")
	region := flag.String("region", getenv("AWS_REGION", "us-east-1"), "AWS region")
	endpoint := flag.String("endpoint", os.Getenv("AWS_ENDPOINT_URL"),
		"override endpoint (LocalStack)")
	flag.Parse()
	if *feedAddr == "" || *topic == "" {
		log.Fatal("feed-sns: -feed and -topic are required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	cfg, err := config.LoadDefaultConfig(ctx, config.WithRegion(*region))
	if err != nil {
		log.Fatalf("feed-sns: aws config: %v", err)
	}
	client := sns.NewFromConfig(cfg, func(o *sns.Options) {
		if *endpoint != "" {
			o.BaseEndpoint = aws.String(*endpoint)
		}
	})
	sink := &snsSink{client: client, topic: *topic}
	if err := feed.Run(ctx, feed.Options{Addr: *feedAddr}, sink); err != nil {
		log.Fatalf("feed-sns: %v", err)
	}
}

type snsSink struct {
	client *sns.Client
	topic  string
}

func (s *snsSink) Publish(ctx context.Context, u feed.Update) error {
	_, err := s.client.Publish(ctx, &sns.PublishInput{
		TopicArn: aws.String(s.topic),
		Message:  aws.String(string(u.Raw)),
		MessageAttributes: map[string]types.MessageAttributeValue{
			"symbol": {DataType: aws.String("String"),
				StringValue: aws.String(u.Symbol)},
		},
	})
	return err
}
func (s *snsSink) Close() error { return nil }

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}
