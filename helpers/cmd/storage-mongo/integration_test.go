//go:build integration

// Integration test for the MongoDB storage helper. Requires a running mongod
// reachable via $CODICIS_MONGO_URI; skipped otherwise. Run with:
//
//	CODICIS_MONGO_URI='mongodb://localhost:27017' \
//	  go test -tags integration ./cmd/storage-mongo/
package main

import (
	"context"
	"os"
	"testing"

	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"

	"github.com/garana/codicis/helpers/internal/storage"
)

func TestMongoStore(t *testing.T) {
	uri := os.Getenv("CODICIS_MONGO_URI")
	if uri == "" {
		t.Skip("set CODICIS_MONGO_URI to run")
	}
	ctx := context.Background()
	client, err := mongo.Connect(ctx, options.Client().ApplyURI(uri))
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer client.Disconnect(ctx) //nolint:errcheck
	db := client.Database("codicis_it")
	if err := db.Drop(ctx); err != nil {
		t.Fatalf("drop: %v", err)
	}
	s := newMongoStore(db)
	if err := s.ensureIndexes(ctx); err != nil {
		t.Fatalf("indexes: %v", err)
	}
	// Apply the $jsonSchema validators so the writes below must satisfy them.
	if err := s.applyValidators(ctx); err != nil {
		t.Fatalf("validators: %v", err)
	}
	const minI64 = -9223372036854775808

	must(t, s.ReportOrder(ctx, storage.Order{Symbol: "BTC", ID: 1, Owner: "u1", Side: "sell", Price: 105, Qty: 8}))
	must(t, s.ReportRest(ctx, storage.RestingOrder{Symbol: "BTC", ID: 1, Side: "sell", Price: 105, Leaves: 8, Seq: 1}))
	must(t, s.ReportOrder(ctx, storage.Order{Symbol: "BTC", ID: 2, Owner: "u1", Side: "sell", Price: 105, Qty: 4}))
	must(t, s.ReportRest(ctx, storage.RestingOrder{Symbol: "BTC", ID: 2, Side: "sell", Price: 105, Leaves: 4, Seq: 2}))

	asks, err := s.PullLevels(ctx, "BTC", "sell", minI64, 10)
	if err != nil || len(asks) != 2 {
		t.Fatalf("asks: %+v err=%v", asks, err)
	}
	// Same-level ordering is by seq: order 1 then order 2.
	if asks[0].ID != 1 || asks[1].ID != 2 {
		t.Fatalf("seq order: %+v", asks)
	}
	must(t, s.ReportFill(ctx, storage.Fill{Symbol: "BTC", ID: 1, Qty: 8, Remaining: 0, Complete: true}))
	asks, _ = s.PullLevels(ctx, "BTC", "sell", minI64, 10)
	if len(asks) != 1 || asks[0].ID != 2 {
		t.Fatalf("after fill asks: %+v", asks)
	}
	if net, _ := s.PullPosition(ctx, "u1", "BTC"); net != -8 {
		t.Fatalf("position: %d", net)
	}

	// The validator must reject a malformed write (a string price), which is
	// exactly the class of bug a schemaless store would otherwise accept.
	_, badErr := s.resting.InsertOne(ctx, map[string]interface{}{
		"_id": "BAD|9", "symbol": "BTC", "side": "sell",
		"price": "not-a-number", "leaves": int64(1), "seq": int64(9)})
	if badErr == nil {
		t.Fatal("expected the $jsonSchema validator to reject a string price")
	}
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatalf("op: %v", err)
	}
}
