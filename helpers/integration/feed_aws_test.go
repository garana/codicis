//go:build integration

package integration

import (
	"context"
	"fmt"
	"testing"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/sns"
	"github.com/aws/aws-sdk-go-v2/service/sqs"
	sqstypes "github.com/aws/aws-sdk-go-v2/service/sqs/types"
	"github.com/testcontainers/testcontainers-go/modules/localstack"
)

// startLocalStack brings up LocalStack and returns its endpoint, having set the
// AWS_* env the bridge (a child process) and the SDK both read.
func startLocalStack(t *testing.T) string {
	t.Helper()
	ctx := context.Background()
	c, err := localstack.Run(ctx, "localstack/localstack:3")
	if err != nil {
		t.Fatalf("start localstack: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	host, _ := c.Host(ctx)
	port, _ := c.MappedPort(ctx, "4566/tcp")
	endpoint := fmt.Sprintf("http://%s:%s", host, port.Port())
	t.Setenv("AWS_ENDPOINT_URL", endpoint)
	t.Setenv("AWS_REGION", "us-east-1")
	t.Setenv("AWS_ACCESS_KEY_ID", "test")
	t.Setenv("AWS_SECRET_ACCESS_KEY", "test")
	return endpoint
}

func awsCfg(t *testing.T, endpoint string) aws.Config {
	t.Helper()
	cfg, err := config.LoadDefaultConfig(context.Background(),
		config.WithRegion("us-east-1"))
	if err != nil {
		t.Fatalf("aws config: %v", err)
	}
	return cfg
}

// receiveSQS collects unique "<symbol-attr>\t<body>" messages from a queue.
func receiveSQS(t *testing.T, sqsc *sqs.Client, queueURL string) []byte {
	t.Helper()
	ctx := context.Background()
	return collect(t, func() (string, bool) {
		out, err := sqsc.ReceiveMessage(ctx, &sqs.ReceiveMessageInput{
			QueueUrl:              aws.String(queueURL),
			MaxNumberOfMessages:   10,
			WaitTimeSeconds:       1,
			MessageAttributeNames: []string{"All"},
		})
		if err != nil || len(out.Messages) == 0 {
			return "", false
		}
		// Return the last; collect dedups, so returning one per call is fine.
		m := out.Messages[len(out.Messages)-1]
		sym := ""
		if a, ok := m.MessageAttributes["symbol"]; ok && a.StringValue != nil {
			sym = *a.StringValue
		}
		body := ""
		if m.Body != nil {
			body = *m.Body
		}
		return fmt.Sprintf("%s\t%s", sym, body), true
	})
}

func TestFeedSQS(t *testing.T) {
	endpoint := startLocalStack(t)
	cfg := awsCfg(t, endpoint)
	sqsc := sqs.NewFromConfig(cfg, func(o *sqs.Options) { o.BaseEndpoint = aws.String(endpoint) })

	q, err := sqsc.CreateQueue(context.Background(),
		&sqs.CreateQueueInput{QueueName: aws.String("codicis-md")})
	if err != nil {
		t.Fatalf("create queue: %v", err)
	}

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-sqs", "-feed", src, "-queue", *q.QueueUrl, "-endpoint", endpoint)

	snapshot(t, "feed-sqs", receiveSQS(t, sqsc, *q.QueueUrl))
}

func TestFeedSNS(t *testing.T) {
	endpoint := startLocalStack(t)
	cfg := awsCfg(t, endpoint)
	snsc := sns.NewFromConfig(cfg, func(o *sns.Options) { o.BaseEndpoint = aws.String(endpoint) })
	sqsc := sqs.NewFromConfig(cfg, func(o *sqs.Options) { o.BaseEndpoint = aws.String(endpoint) })
	ctx := context.Background()

	topic, err := snsc.CreateTopic(ctx, &sns.CreateTopicInput{Name: aws.String("codicis-md")})
	if err != nil {
		t.Fatalf("create topic: %v", err)
	}
	q, err := sqsc.CreateQueue(ctx, &sqs.CreateQueueInput{QueueName: aws.String("codicis-md-sns-q")})
	if err != nil {
		t.Fatalf("create queue: %v", err)
	}
	attr, err := sqsc.GetQueueAttributes(ctx, &sqs.GetQueueAttributesInput{
		QueueUrl:       q.QueueUrl,
		AttributeNames: []sqstypes.QueueAttributeName{sqstypes.QueueAttributeNameQueueArn},
	})
	if err != nil {
		t.Fatalf("queue attrs: %v", err)
	}
	if _, err := snsc.Subscribe(ctx, &sns.SubscribeInput{
		TopicArn: topic.TopicArn, Protocol: aws.String("sqs"),
		Endpoint: aws.String(attr.Attributes["QueueArn"]), ReturnSubscriptionArn: true,
		Attributes: map[string]string{"RawMessageDelivery": "true"},
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	src := startFeedSource(t, feedLines)
	runBridge(t, "feed-sns", "-feed", src, "-topic", *topic.TopicArn, "-endpoint", endpoint)

	snapshot(t, "feed-sns", receiveSQS(t, sqsc, *q.QueueUrl))
}
