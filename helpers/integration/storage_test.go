//go:build integration

// Black-box integration tests for the storage helper binaries (postgres, mysql,
// mongo). Each brings up a real datastore on an ephemeral, Docker-assigned port
// via testcontainers-go, os/exec's the BUILT helper binary against it, drives a
// fixed sequence of wire requests over stdin, and snapshots the response stream
// the helper emits (which is backend-agnostic -- the same wire protocol).
package integration

import (
	"bytes"
	"context"
	"io"
	"math"
	"os"
	"os/exec"
	"strconv"
	"testing"
	"time"

	"github.com/testcontainers/testcontainers-go/modules/mongodb"
	"github.com/testcontainers/testcontainers-go/modules/mysql"
	"github.com/testcontainers/testcontainers-go/modules/postgres"

	"github.com/garana/codicis/helpers/internal/wire"
)

// helperBin returns the path to a built helper binary, failing if it is missing.
func helperBin(t *testing.T, name string) string {
	t.Helper()
	if p := os.Getenv("CODICIS_" + envName(name) + "_BIN"); p != "" {
		return p
	}
	p := "../bin/" + name
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("%s binary not found at %q (run `make -C helpers build`): %v", name, p, err)
	}
	return p
}

func envName(name string) string {
	b := make([]byte, 0, len(name))
	for _, c := range name {
		if c == '-' {
			b = append(b, '_')
		} else if c >= 'a' && c <= 'z' {
			b = append(b, byte(c-'a'+'A'))
		} else {
			b = append(b, byte(c))
		}
	}
	return string(b)
}

// driveStorage runs the fixed request sequence against a storage helper binary
// and returns the raw wire response stream it emitted (deterministic: req_ids
// are ours, values are fixed).
func driveStorage(t *testing.T, bin string, args ...string) []byte {
	t.Helper()
	ctx := context.Background()
	cmd := exec.CommandContext(ctx, bin, args...)
	cmd.Stderr = os.Stderr
	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatalf("stdin pipe: %v", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("stdout pipe: %v", err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start %s: %v", bin, err)
	}
	t.Cleanup(func() { _ = stdin.Close(); _ = cmd.Wait() })

	var captured bytes.Buffer
	r := wire.NewReader(io.TeeReader(stdout, &captured))

	// Strict ping-pong so each write is applied before the dependent read; the
	// TeeReader records the raw response bytes for the snapshot.
	send := func(m *wire.Message) {
		t.Helper()
		if _, err := stdin.Write(wire.Encode(m)); err != nil {
			t.Fatalf("write %s: %v", m.Type, err)
		}
		if _, err := r.ReadMessage(); err != nil {
			t.Fatalf("read response to %s: %v", m.Type, err)
		}
	}

	o := &wire.Message{ReqID: 1, Type: "report_order"}
	o.Set("symbol", "BTC")
	o.Set("owner", "")
	o.SetInt("id", 1)
	o.Set("side", "sell")
	o.SetInt("price", 105)
	o.SetInt("qty", 8)
	send(o)

	rr := &wire.Message{ReqID: 2, Type: "report_rest"}
	rr.Set("symbol", "BTC")
	rr.Set("side", "sell")
	rr.SetInt("id", 1)
	rr.SetInt("price", 105)
	rr.SetInt("leaves", 8)
	rr.SetInt("seq", 1)
	send(rr)

	pl := &wire.Message{ReqID: 3, Type: "pull_levels"}
	pl.Set("symbol", "BTC")
	pl.Set("side", "sell")
	pl.SetInt("from_price", math.MinInt64)
	pl.SetInt("count", 10)
	send(pl)

	f := &wire.Message{ReqID: 4, Type: "report_fill"}
	f.Set("symbol", "BTC")
	f.SetInt("id", 1)
	f.SetInt("qty", 3)
	f.SetInt("remaining", 5)
	send(f)

	pl2 := &wire.Message{ReqID: 5, Type: "pull_levels"}
	pl2.Set("symbol", "BTC")
	pl2.Set("side", "sell")
	pl2.SetInt("from_price", math.MinInt64)
	pl2.SetInt("count", 10)
	send(pl2)

	pp := &wire.Message{ReqID: 6, Type: "pull_position"}
	pp.Set("user", "")
	pp.Set("symbol", "BTC")
	send(pp)

	return captured.Bytes()
}

func TestStoragePostgres(t *testing.T) {
	ctx := context.Background()
	c, err := postgres.Run(ctx, "postgres:16-alpine",
		postgres.WithDatabase("codicis"),
		postgres.WithUsername("codicis"),
		postgres.WithPassword("codicis"),
		postgres.BasicWaitStrategies(),
	)
	if err != nil {
		t.Fatalf("start postgres: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	dsn, err := c.ConnectionString(ctx, "sslmode=disable")
	if err != nil {
		t.Fatalf("dsn: %v", err)
	}
	got := driveStorage(t, helperBin(t, "storage-postgres"), "-dsn", dsn, "-migrate")
	snapshot(t, "storage-postgres", got)
}

func TestStorageMySQL(t *testing.T) {
	ctx := context.Background()
	c, err := mysql.Run(ctx, "mysql:8.4",
		mysql.WithDatabase("codicis"),
		mysql.WithUsername("codicis"),
		mysql.WithPassword("codicis"),
	)
	if err != nil {
		t.Fatalf("start mysql: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	dsn, err := c.ConnectionString(ctx)
	if err != nil {
		t.Fatalf("dsn: %v", err)
	}
	got := driveStorage(t, helperBin(t, "storage-mysql"), "-dsn", dsn, "-migrate")
	snapshot(t, "storage-mysql", got)
}

func TestStorageMongo(t *testing.T) {
	ctx := context.Background()
	c, err := mongodb.Run(ctx, "mongo:7")
	if err != nil {
		t.Fatalf("start mongo: %v", err)
	}
	t.Cleanup(func() { terminate(c) })
	uri, err := c.ConnectionString(ctx)
	if err != nil {
		t.Fatalf("uri: %v", err)
	}
	got := driveStorage(t, helperBin(t, "storage-mongo"),
		"-uri", uri, "-db", "codicis", "-migrate")
	snapshot(t, "storage-mongo", got)
}

// terminate is a testcontainers container with a Terminate method.
type terminable interface {
	Terminate(context.Context, ...any) error
}

func terminate(c any) {
	if tc, ok := c.(terminable); ok {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		_ = tc.Terminate(ctx)
	}
}

// ensure strconv stays imported if the sequence is trimmed during edits.
var _ = strconv.Itoa
