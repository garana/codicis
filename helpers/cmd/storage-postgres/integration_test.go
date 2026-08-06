//go:build integration

// Integration test for the PostgreSQL storage helper. Requires a running
// PostgreSQL reachable via $CODICIS_PG_DSN; skipped otherwise. Run with:
//
//	CODICIS_PG_DSN=postgres://codicis:codicis@localhost:5432/codicis?sslmode=disable \
//	  go test -tags integration ./cmd/storage-postgres/
package main

import (
	"context"
	"os"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/garana/codicis/helpers/internal/storage"
	"github.com/garana/codicis/helpers/schema"
)

func TestPostgresStore(t *testing.T) {
	dsn := os.Getenv("CODICIS_PG_DSN")
	if dsn == "" {
		t.Skip("set CODICIS_PG_DSN to run")
	}
	ctx := context.Background()
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer pool.Close()
	if _, err := pool.Exec(ctx, schema.Postgres); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tbl := range []string{"orders", "resting", "positions", "fills", "trades"} {
		if _, err := pool.Exec(ctx, "TRUNCATE "+tbl); err != nil {
			t.Fatalf("truncate %s: %v", tbl, err)
		}
	}

	s := &pgStore{pool: pool}

	// A resting sell at 105 and buy at 101, both owned by u1.
	must(t, s.ReportOrder(ctx, storage.Order{Symbol: "BTC", ID: 1, Owner: "u1", Side: "sell", Price: 105, Qty: 8}))
	must(t, s.ReportRest(ctx, storage.RestingOrder{Symbol: "BTC", ID: 1, Side: "sell", Price: 105, Leaves: 8, Seq: 1}))
	must(t, s.ReportOrder(ctx, storage.Order{Symbol: "BTC", ID: 2, Owner: "u1", Side: "buy", Price: 101, Qty: 3}))
	must(t, s.ReportRest(ctx, storage.RestingOrder{Symbol: "BTC", ID: 2, Side: "buy", Price: 101, Leaves: 3, Seq: 2}))

	// pull_levels: the sell side, from INT64_MIN so all asks come back.
	asks, err := s.PullLevels(ctx, "BTC", "sell", -9223372036854775808, 10)
	if err != nil {
		t.Fatalf("pull_levels: %v", err)
	}
	if len(asks) != 1 || asks[0].ID != 1 || asks[0].Leaves != 8 {
		t.Fatalf("asks: %+v", asks)
	}

	// A partial fill of the sell: 3 of 8, remaining 5.
	must(t, s.ReportFill(ctx, storage.Fill{Symbol: "BTC", ID: 1, Qty: 3, Remaining: 5, Complete: false}))
	asks, _ = s.PullLevels(ctx, "BTC", "sell", -9223372036854775808, 10)
	if len(asks) != 1 || asks[0].Leaves != 5 {
		t.Fatalf("after fill asks: %+v", asks)
	}
	// The sell shrank the owner's net position (short 3).
	net, _ := s.PullPosition(ctx, "u1", "BTC")
	if net != -3 {
		t.Fatalf("position: %d", net)
	}

	// Complete the sell: it leaves the book.
	must(t, s.ReportFill(ctx, storage.Fill{Symbol: "BTC", ID: 1, Qty: 5, Remaining: 0, Complete: true}))
	asks, _ = s.PullLevels(ctx, "BTC", "sell", -9223372036854775808, 10)
	if len(asks) != 0 {
		t.Fatalf("expected empty asks, got %+v", asks)
	}
	net, _ = s.PullPosition(ctx, "u1", "BTC")
	if net != -8 {
		t.Fatalf("final position: %d", net)
	}

	// Cancel the resting buy.
	must(t, s.ReportCancel(ctx, "BTC", 2))
	bids, _ := s.PullLevels(ctx, "BTC", "buy", 9223372036854775807, 10)
	if len(bids) != 0 {
		t.Fatalf("expected empty bids, got %+v", bids)
	}
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("op: %v", err)
	}
}
