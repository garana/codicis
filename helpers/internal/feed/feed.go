// Package feed provides the shared runtime for the market-data fan-out bridges.
//
// A bridge is a TCP subscriber of the codicis feed-helper: it reads the
// helper's newline-delimited JSON stream (an L1 snapshot on connect, then L1
// updates and any l2/l2d/l3 query replies) and republishes each per-symbol
// message to a broker via a Sink. Each bridge binary supplies only a Sink
// (Kafka, NATS, Redis, ...), so this connect/read/reconnect loop is written
// once.
//
// The feed is best-effort: the helper drops a slow subscriber, so on any read
// error the bridge reconnects (and the helper re-snapshots).
package feed

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"time"
)

// Update is one message relayed from the feed-helper.
type Update struct {
	Symbol string // the "sym" field ("" for messages without one, e.g. ready)
	Type   string // the "t" field: l1, l2, l2d, l3, ready, ...
	Raw    []byte // the raw JSON line (without the trailing newline)
}

// Sink republishes updates to a broker. Implementations must be safe for the
// single goroutine that drives Run.
type Sink interface {
	// Publish sends one update. Returning an error aborts the run.
	Publish(ctx context.Context, u Update) error
	Close() error
}

// Options configures the subscriber loop.
type Options struct {
	Addr      string        // feed-helper "host:port"
	Reconnect time.Duration // delay between reconnect attempts (default 1s)
	// PublishReady, when true, forwards the {"t":"ready"} greeting too; by
	// default only messages carrying a symbol are published.
	PublishReady bool
}

// envelope is the minimal shape parsed from each JSON line to route it.
type envelope struct {
	Type   string `json:"t"`
	Symbol string `json:"sym"`
}

// Run connects to the feed-helper and republishes messages to sink until ctx is
// cancelled. It reconnects on any connection error.
func Run(ctx context.Context, opts Options, sink Sink) error {
	reconnect := opts.Reconnect
	if reconnect <= 0 {
		reconnect = time.Second
	}
	for {
		if err := ctx.Err(); err != nil {
			return nil
		}
		if err := stream(ctx, opts, sink); err != nil {
			// A Sink error is terminal; a connection error just triggers a
			// reconnect after a short delay.
			if _, ok := err.(sinkError); ok {
				return err.(sinkError).err
			}
		}
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(reconnect):
		}
	}
}

type sinkError struct{ err error }

func (e sinkError) Error() string { return e.err.Error() }

// stream handles one connection: read lines until it drops.
func stream(ctx context.Context, opts Options, sink Sink) error {
	var d net.Dialer
	conn, err := d.DialContext(ctx, "tcp", opts.Addr)
	if err != nil {
		return err
	}
	defer conn.Close()

	sc := bufio.NewScanner(conn)
	sc.Buffer(make([]byte, 0, 64*1024), 16*1024*1024)
	for sc.Scan() {
		if err := ctx.Err(); err != nil {
			return nil
		}
		line := sc.Bytes()
		if len(line) == 0 {
			continue
		}
		var env envelope
		if json.Unmarshal(line, &env) != nil {
			continue // ignore non-JSON / partial lines
		}
		if env.Symbol == "" && !opts.PublishReady {
			continue
		}
		// Copy the line: the scanner reuses its buffer.
		raw := make([]byte, len(line))
		copy(raw, line)
		if perr := sink.Publish(ctx, Update{
			Symbol: env.Symbol, Type: env.Type, Raw: raw,
		}); perr != nil {
			return sinkError{perr}
		}
	}
	return sc.Err()
}
