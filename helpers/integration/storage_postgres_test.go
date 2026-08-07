//go:build integration

// Black-box integration test for the storage-postgres helper binary.
//
// testcontainers-go brings up a real PostgreSQL on an ephemeral, Docker-assigned
// port (so it never collides with a host postgres/mongod), then this test
// os/exec's the BUILT storage-postgres binary against it and drives the actual
// text wire protocol over the child's stdin/stdout -- exercising the shipped
// binary end to end (flags, -migrate, schema, the wire codec, and the store).
//
// The binary is located via $CODICIS_STORAGE_POSTGRES_BIN, defaulting to
// ../bin/storage-postgres (so `make -C helpers build` is a prerequisite).
package integration

import (
	"bufio"
	"context"
	"math"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/garana/codicis/helpers/internal/wire"
	"github.com/testcontainers/testcontainers-go/modules/postgres"
)

func TestStoragePostgresBlackBox(t *testing.T) {
	ctx := context.Background()

	pg, err := postgres.Run(ctx, "postgres:16-alpine",
		postgres.WithDatabase("codicis"),
		postgres.WithUsername("codicis"),
		postgres.WithPassword("codicis"),
		postgres.BasicWaitStrategies(),
	)
	if err != nil {
		t.Fatalf("start postgres: %v", err)
	}
	t.Cleanup(func() {
		cctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		_ = pg.Terminate(cctx)
	})

	dsn, err := pg.ConnectionString(ctx, "sslmode=disable")
	if err != nil {
		t.Fatalf("dsn: %v", err)
	}

	bin := os.Getenv("CODICIS_STORAGE_POSTGRES_BIN")
	if bin == "" {
		bin = "../bin/storage-postgres"
	}
	if _, err := os.Stat(bin); err != nil {
		t.Fatalf("storage-postgres binary not found at %q (run `make -C helpers build`): %v", bin, err)
	}

	cmd := exec.CommandContext(ctx, bin, "-dsn", dsn, "-migrate")
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
		t.Fatalf("start helper: %v", err)
	}
	t.Cleanup(func() { _ = stdin.Close(); _ = cmd.Wait() })

	r := wire.NewReader(bufio.NewReader(stdout))

	// Strict ping-pong: send one request, read its (req_id-matched) response, so
	// each write is durably applied before the dependent read is issued.
	roundtrip := func(m *wire.Message) *wire.Message {
		t.Helper()
		if _, err := stdin.Write(wire.Encode(m)); err != nil {
			t.Fatalf("write %s: %v", m.Type, err)
		}
		resp, err := r.ReadMessage()
		if err != nil {
			t.Fatalf("read response to %s: %v", m.Type, err)
		}
		if resp.ReqID != m.ReqID {
			t.Fatalf("response req_id=%d, want %d (type %s)", resp.ReqID, m.ReqID, resp.Type)
		}
		return resp
	}
	mustOK := func(resp *wire.Message) {
		t.Helper()
		if st, _ := resp.Get("status"); st != "ok" {
			reason, _ := resp.Get("reason")
			t.Fatalf("%s: status=%q reason=%q", resp.Type, st, reason)
		}
	}

	// Report a resting sell (id=1, 8 @ 105).
	o := &wire.Message{ReqID: 1, Type: "report_order"}
	o.Set("symbol", "BTC")
	o.Set("owner", "")
	o.SetInt("id", 1)
	o.Set("side", "sell")
	o.SetInt("price", 105)
	o.SetInt("qty", 8)
	mustOK(roundtrip(o))

	rr := &wire.Message{ReqID: 2, Type: "report_rest"}
	rr.Set("symbol", "BTC")
	rr.Set("side", "sell")
	rr.SetInt("id", 1)
	rr.SetInt("price", 105)
	rr.SetInt("leaves", 8)
	rr.SetInt("seq", 1)
	mustOK(roundtrip(rr))

	// pull_levels on the sell side must return that one resting order.
	pl := &wire.Message{ReqID: 3, Type: "pull_levels"}
	pl.Set("symbol", "BTC")
	pl.Set("side", "sell")
	pl.SetInt("from_price", math.MinInt64)
	pl.SetInt("count", 10)
	resp := roundtrip(pl)
	orders, _ := resp.Get("orders")
	// Blob is "id,price,leaves,seq" records joined by ';'.
	if !strings.Contains(orders, "1,105,8,1") {
		t.Fatalf("pull_levels orders=%q, want a 1,105,8,1 record", orders)
	}
	if c, _ := resp.Get("count"); c != "1" {
		t.Fatalf("pull_levels count=%q, want 1", c)
	}

	// A partial fill shrinks the resting leaves 8 -> 5.
	f := &wire.Message{ReqID: 4, Type: "report_fill"}
	f.Set("symbol", "BTC")
	f.SetInt("id", 1)
	f.SetInt("qty", 3)
	f.SetInt("remaining", 5)
	mustOK(roundtrip(f))

	pl2 := &wire.Message{ReqID: 5, Type: "pull_levels"}
	pl2.Set("symbol", "BTC")
	pl2.Set("side", "sell")
	pl2.SetInt("from_price", math.MinInt64)
	pl2.SetInt("count", 10)
	resp2 := roundtrip(pl2)
	orders2, _ := resp2.Get("orders")
	if !strings.Contains(orders2, "1,105,5,1") {
		t.Fatalf("after fill orders=%q, want leaves 5", orders2)
	}
}
