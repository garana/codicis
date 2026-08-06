//go:build integration

// Integration test for the MySQL storage helper. Requires a running MySQL
// reachable via $CODICIS_MYSQL_DSN; skipped otherwise. Run with:
//
//	CODICIS_MYSQL_DSN='codicis:codicis@tcp(localhost:3306)/codicis' \
//	  go test -tags integration ./cmd/storage-mysql/
package main

import (
	"context"
	"database/sql"
	"os"
	"testing"

	_ "github.com/go-sql-driver/mysql"

	"github.com/garana/codicis/helpers/internal/storage"
	"github.com/garana/codicis/helpers/schema"
)

func TestMySQLStore(t *testing.T) {
	dsn := os.Getenv("CODICIS_MYSQL_DSN")
	if dsn == "" {
		t.Skip("set CODICIS_MYSQL_DSN to run")
	}
	ctx := context.Background()
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()
	if err := applySchema(ctx, db, schema.MySQL); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	for _, tbl := range []string{"orders", "resting", "positions", "fills", "trades"} {
		if _, err := db.ExecContext(ctx, "TRUNCATE TABLE "+tbl); err != nil {
			t.Fatalf("truncate %s: %v", tbl, err)
		}
	}

	s := &myStore{db: db}
	const minI64 = -9223372036854775808
	const maxI64 = 9223372036854775807

	must(t, s.ReportOrder(ctx, storage.Order{Symbol: "BTC", ID: 1, Owner: "u1", Side: "sell", Price: 105, Qty: 8}))
	must(t, s.ReportRest(ctx, storage.RestingOrder{Symbol: "BTC", ID: 1, Side: "sell", Price: 105, Leaves: 8, Seq: 1}))

	asks, err := s.PullLevels(ctx, "BTC", "sell", minI64, 10)
	if err != nil || len(asks) != 1 || asks[0].Leaves != 8 {
		t.Fatalf("asks: %+v err=%v", asks, err)
	}
	must(t, s.ReportFill(ctx, storage.Fill{Symbol: "BTC", ID: 1, Qty: 3, Remaining: 5, Complete: false}))
	if net, _ := s.PullPosition(ctx, "u1", "BTC"); net != -3 {
		t.Fatalf("position: %d", net)
	}
	must(t, s.ReportFill(ctx, storage.Fill{Symbol: "BTC", ID: 1, Qty: 5, Remaining: 0, Complete: true}))
	asks, _ = s.PullLevels(ctx, "BTC", "sell", minI64, 10)
	if len(asks) != 0 {
		t.Fatalf("expected empty asks, got %+v", asks)
	}
	if net, _ := s.PullPosition(ctx, "u1", "BTC"); net != -8 {
		t.Fatalf("final position: %d", net)
	}
	_ = maxI64
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("op: %v", err)
	}
}
