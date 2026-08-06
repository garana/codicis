// Command storage-mongo is a reference codicis storage helper backed by
// MongoDB. It speaks the storage-helper wire protocol on stdin/stdout.
//
// Collections (database "codicis"): orders, resting, positions (keyed by a
// composite string _id), and append-only fills / trades. Multi-document
// transactions are NOT used, so it runs against a standalone mongod; a
// production deployment would use a replica set and wrap ReportFill in a
// transaction.
//
// Config (flags override env):
//
//	-uri / $CODICIS_MONGO_URI   e.g. mongodb://localhost:27017
//	-db                         database name (default "codicis")
//	-read-concurrency           max concurrent Pull* reads (default 8)
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"

	"github.com/garana/codicis/helpers/internal/storage"
)

func main() {
	uri := flag.String("uri", os.Getenv("CODICIS_MONGO_URI"), "MongoDB URI")
	dbName := flag.String("db", "codicis", "database name")
	migrate := flag.Bool("migrate", false,
		"apply $jsonSchema validators on startup (idempotent)")
	readConc := flag.Int("read-concurrency", 8, "max concurrent Pull* reads")
	flag.Parse()
	if *uri == "" {
		log.Fatal("storage-mongo: -uri or $CODICIS_MONGO_URI is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	client, err := mongo.Connect(ctx, options.Client().ApplyURI(*uri))
	if err != nil {
		log.Fatalf("storage-mongo: connect: %v", err)
	}
	defer client.Disconnect(context.Background()) //nolint:errcheck
	if err := client.Ping(ctx, nil); err != nil {
		log.Fatalf("storage-mongo: ping: %v", err)
	}

	store := newMongoStore(client.Database(*dbName))
	if err := store.ensureIndexes(ctx); err != nil {
		log.Fatalf("storage-mongo: indexes: %v", err)
	}
	if *migrate {
		if err := store.applyValidators(ctx); err != nil {
			log.Fatalf("storage-mongo: validators: %v", err)
		}
	}
	srv := storage.NewServer(store, os.Stdin, os.Stdout, *readConc)
	if err := srv.Run(ctx); err != nil {
		log.Fatalf("storage-mongo: %v", err)
	}
}

type mongoStore struct {
	db        *mongo.Database
	orders    *mongo.Collection
	resting   *mongo.Collection
	positions *mongo.Collection
	fills     *mongo.Collection
	trades    *mongo.Collection
}

func newMongoStore(db *mongo.Database) *mongoStore {
	return &mongoStore{
		db:        db,
		orders:    db.Collection("orders"),
		resting:   db.Collection("resting"),
		positions: db.Collection("positions"),
		fills:     db.Collection("fills"),
		trades:    db.Collection("trades"),
	}
}

// key composes the (symbol, id) primary key as a single string _id.
func key(symbol string, id uint64) string {
	return symbol + "|" + strconv.FormatUint(id, 10)
}

// num is the accepted BSON types for a 64-bit numeric field. A wide-precision
// (128/256-bit) deployment would use a separate collection whose validator
// accepts "decimal" instead (see task: multi-precision DB mirrors C++ widths).
var num = bson.A{"int", "long"}

// validators returns the $jsonSchema validator per collection, mirroring what
// the helper writes. Kept in sync with schema/mongo.js.
func validators() map[string]bson.M {
	sideEnum := bson.M{"enum": bson.A{"buy", "sell"}}
	strT := bson.M{"bsonType": "string"}
	numT := bson.M{"bsonType": num}
	obj := func(required bson.A, props bson.M) bson.M {
		return bson.M{"$jsonSchema": bson.M{"bsonType": "object",
			"required": required, "properties": props}}
	}
	return map[string]bson.M{
		"orders": obj(bson.A{"_id", "symbol", "owner", "side", "price", "qty"},
			bson.M{"_id": strT, "symbol": strT, "owner": strT,
				"side": sideEnum, "price": numT, "qty": numT}),
		"resting": obj(bson.A{"_id", "symbol", "side", "price", "leaves", "seq"},
			bson.M{"_id": strT, "symbol": strT, "side": sideEnum,
				"price": numT, "leaves": numT, "seq": numT}),
		"positions": obj(bson.A{"_id", "owner", "symbol", "net"},
			bson.M{"_id": strT, "owner": strT, "symbol": strT, "net": numT}),
		"fills": obj(bson.A{"symbol", "id", "qty", "remaining", "complete"},
			bson.M{"symbol": strT, "id": numT, "qty": numT, "remaining": numT,
				"complete": bson.M{"bsonType": "bool"}}),
		"trades": obj(bson.A{"symbol", "taker", "maker", "price", "qty"},
			bson.M{"symbol": strT, "taker": numT, "maker": numT,
				"price": numT, "qty": numT}),
	}
}

// applyValidators installs (or updates) the $jsonSchema validator on each
// collection. "moderate" level validates inserts and updates to already-valid
// documents, so it never rejects on pre-existing data; "error" rejects a write
// that violates the schema.
func (s *mongoStore) applyValidators(ctx context.Context) error {
	for name, v := range validators() {
		// Ensure the collection exists (ignore "already exists").
		_ = s.db.CreateCollection(ctx, name)
		cmd := bson.D{{Key: "collMod", Value: name}, {Key: "validator", Value: v},
			{Key: "validationLevel", Value: "moderate"},
			{Key: "validationAction", Value: "error"}}
		if err := s.db.RunCommand(ctx, cmd).Err(); err != nil {
			return err
		}
	}
	return nil
}

func (s *mongoStore) ensureIndexes(ctx context.Context) error {
	// Serve pull_levels: filter by symbol+side+price, ordered by price+seq.
	_, err := s.resting.Indexes().CreateOne(ctx, mongo.IndexModel{
		Keys: bson.D{{Key: "symbol", Value: 1}, {Key: "side", Value: 1},
			{Key: "price", Value: 1}, {Key: "seq", Value: 1}},
	})
	return err
}

func (s *mongoStore) ReportOrder(ctx context.Context, o storage.Order) error {
	_, err := s.orders.ReplaceOne(ctx, bson.M{"_id": key(o.Symbol, o.ID)},
		bson.M{"_id": key(o.Symbol, o.ID), "symbol": o.Symbol, "owner": o.Owner,
			"side": o.Side, "price": o.Price, "qty": o.Qty},
		options.Replace().SetUpsert(true))
	return err
}

func (s *mongoStore) ReportRest(ctx context.Context, r storage.RestingOrder) error {
	_, err := s.resting.ReplaceOne(ctx, bson.M{"_id": key(r.Symbol, r.ID)},
		bson.M{"_id": key(r.Symbol, r.ID), "symbol": r.Symbol, "side": r.Side,
			"price": r.Price, "leaves": r.Leaves, "seq": int64(r.Seq)},
		options.Replace().SetUpsert(true))
	return err
}

func (s *mongoStore) ReportFill(ctx context.Context, f storage.Fill) error {
	if _, err := s.fills.InsertOne(ctx, bson.M{"symbol": f.Symbol,
		"id": int64(f.ID), "qty": f.Qty, "remaining": f.Remaining,
		"complete": f.Complete}); err != nil {
		return err
	}
	// Attribute the position delta from the order's owner/side.
	var ord struct {
		Owner string `bson:"owner"`
		Side  string `bson:"side"`
	}
	err := s.orders.FindOne(ctx, bson.M{"_id": key(f.Symbol, f.ID)}).Decode(&ord)
	if err == nil && ord.Owner != "" {
		delta := f.Qty
		if ord.Side == "sell" {
			delta = -f.Qty
		}
		if _, err := s.positions.UpdateOne(ctx,
			bson.M{"_id": ord.Owner + "|" + f.Symbol},
			bson.M{"$inc": bson.M{"net": delta},
				"$setOnInsert": bson.M{"owner": ord.Owner, "symbol": f.Symbol}},
			options.Update().SetUpsert(true)); err != nil {
			return err
		}
	} else if err != nil && err != mongo.ErrNoDocuments {
		return err
	}
	if f.Complete {
		_, err = s.resting.DeleteOne(ctx, bson.M{"_id": key(f.Symbol, f.ID)})
	} else {
		_, err = s.resting.UpdateOne(ctx, bson.M{"_id": key(f.Symbol, f.ID)},
			bson.M{"$set": bson.M{"leaves": f.Remaining}})
	}
	return err
}

func (s *mongoStore) ReportCancel(ctx context.Context, symbol string, id uint64) error {
	_, err := s.resting.DeleteOne(ctx, bson.M{"_id": key(symbol, id)})
	return err
}

func (s *mongoStore) ReportTrade(ctx context.Context, t storage.Trade) error {
	_, err := s.trades.InsertOne(ctx, bson.M{"symbol": t.Symbol,
		"taker": int64(t.Taker), "maker": int64(t.Maker),
		"price": t.Price, "qty": t.Qty})
	return err
}

func (s *mongoStore) Commit(_ context.Context) error { return nil }

func (s *mongoStore) PullPosition(ctx context.Context, user, symbol string) (int64, error) {
	var doc struct {
		Net int64 `bson:"net"`
	}
	err := s.positions.FindOne(ctx, bson.M{"_id": user + "|" + symbol}).Decode(&doc)
	if err == mongo.ErrNoDocuments {
		return 0, nil
	}
	if err != nil {
		return 0, err
	}
	return doc.Net, nil
}

func (s *mongoStore) PullLevels(ctx context.Context, symbol, side string, fromPrice int64, count int) ([]storage.RestingOrder, error) {
	// Best price first (buy: price < from, descending; sell: price > from,
	// ascending), seq order within a level; take up to `count` price levels.
	priceCmp := "$gt"
	dir := 1
	if side == "buy" {
		priceCmp = "$lt"
		dir = -1
	}
	filter := bson.M{"symbol": symbol, "side": side,
		"price": bson.M{priceCmp: fromPrice}}
	cur, err := s.resting.Find(ctx, filter, options.Find().SetSort(
		bson.D{{Key: "price", Value: dir}, {Key: "seq", Value: 1}}))
	if err != nil {
		return nil, err
	}
	defer cur.Close(ctx)

	var out []storage.RestingOrder
	levels := 0
	var last int64
	first := true
	for cur.Next(ctx) {
		var doc struct {
			ID     string `bson:"_id"`
			Price  int64  `bson:"price"`
			Leaves int64  `bson:"leaves"`
			Seq    int64  `bson:"seq"`
		}
		if err := cur.Decode(&doc); err != nil {
			return nil, err
		}
		if first || doc.Price != last {
			if count > 0 && levels >= count {
				break // reached the requested number of price levels
			}
			levels++
			last = doc.Price
			first = false
		}
		out = append(out, storage.RestingOrder{
			Symbol: symbol, Side: side, ID: idFromKey(doc.ID),
			Price: doc.Price, Leaves: doc.Leaves, Seq: uint64(doc.Seq)})
	}
	return out, cur.Err()
}

func (s *mongoStore) Close() error { return nil }

// idFromKey extracts the order id from a "symbol|id" composite key.
func idFromKey(k string) uint64 {
	for i := len(k) - 1; i >= 0; i-- {
		if k[i] == '|' {
			n, _ := strconv.ParseUint(k[i+1:], 10, 64)
			return n
		}
	}
	return 0
}
