// Command storage-postgres is a reference codicis storage helper backed by
// PostgreSQL. It speaks the storage-helper wire protocol on stdin/stdout and
// persists to the schema in helpers/schema/postgres.sql.
//
// It is a REFERENCE implementation: an operator may replace it with their own
// (any language) as long as it speaks the same protocol. Writes are applied in
// arrival order; Pull* reads run concurrently on the pool.
//
// Config (flags override env):
//
//	-dsn / $CODICIS_PG_DSN     postgres connection string
//	-migrate                   apply the schema on startup (idempotent)
//	-read-concurrency          max concurrent Pull* reads (default 8)
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/garana/codicis/helpers/internal/storage"
	"github.com/garana/codicis/helpers/schema"
)

func main() {
	dsn := flag.String("dsn", os.Getenv("CODICIS_PG_DSN"),
		"PostgreSQL connection string")
	migrate := flag.Bool("migrate", false, "apply the schema on startup")
	readConc := flag.Int("read-concurrency", 8, "max concurrent Pull* reads")
	flag.Parse()

	if *dsn == "" {
		log.Fatal("storage-postgres: -dsn or $CODICIS_PG_DSN is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	pool, err := pgxpool.New(ctx, *dsn)
	if err != nil {
		log.Fatalf("storage-postgres: connect: %v", err)
	}
	defer pool.Close()

	if *migrate {
		if _, err := pool.Exec(ctx, schema.Postgres); err != nil {
			log.Fatalf("storage-postgres: migrate: %v", err)
		}
	}

	store := &pgStore{pool: pool}
	srv := storage.NewServer(store, os.Stdin, os.Stdout, *readConc)
	if err := srv.Run(ctx); err != nil {
		log.Fatalf("storage-postgres: %v", err)
	}
}

// pgStore implements storage.Store over a pgx connection pool.
type pgStore struct {
	pool *pgxpool.Pool
}

func (s *pgStore) ReportOrder(ctx context.Context, o storage.Order) error {
	_, err := s.pool.Exec(ctx, `
		INSERT INTO orders (symbol, id, owner, side, price, qty)
		VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (symbol, id) DO UPDATE SET
			owner = EXCLUDED.owner, side = EXCLUDED.side,
			price = EXCLUDED.price, qty = EXCLUDED.qty`,
		o.Symbol, o.ID, o.Owner, o.Side, o.Price, o.Qty)
	return err
}

func (s *pgStore) ReportRest(ctx context.Context, r storage.RestingOrder) error {
	_, err := s.pool.Exec(ctx, `
		INSERT INTO resting (symbol, id, side, price, leaves, seq)
		VALUES ($1, $2, $3, $4, $5, $6)
		ON CONFLICT (symbol, id) DO UPDATE SET
			side = EXCLUDED.side, price = EXCLUDED.price,
			leaves = EXCLUDED.leaves, seq = EXCLUDED.seq`,
		r.Symbol, r.ID, r.Side, r.Price, r.Leaves, r.Seq)
	return err
}

func (s *pgStore) ReportFill(ctx context.Context, f storage.Fill) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx) //nolint:errcheck // no-op after a successful commit

	if _, err = tx.Exec(ctx, `
		INSERT INTO fills (symbol, id, qty, remaining, complete)
		VALUES ($1, $2, $3, $4, $5)`,
		f.Symbol, f.ID, f.Qty, f.Remaining, f.Complete); err != nil {
		return err
	}
	// Adjust the owner's net position (buy +, sell -) using the stored order.
	if _, err = tx.Exec(ctx, `
		INSERT INTO positions (owner, symbol, net)
		SELECT o.owner, o.symbol,
		       CASE WHEN o.side = 'buy' THEN $3::bigint ELSE -$3::bigint END
		FROM orders o
		WHERE o.symbol = $1 AND o.id = $2 AND o.owner <> ''
		ON CONFLICT (owner, symbol)
		DO UPDATE SET net = positions.net + EXCLUDED.net`,
		f.Symbol, f.ID, f.Qty); err != nil {
		return err
	}
	// Decrement the resting book; drop the order when fully filled.
	if f.Complete {
		if _, err = tx.Exec(ctx,
			`DELETE FROM resting WHERE symbol = $1 AND id = $2`,
			f.Symbol, f.ID); err != nil {
			return err
		}
	} else {
		if _, err = tx.Exec(ctx,
			`UPDATE resting SET leaves = $3 WHERE symbol = $1 AND id = $2`,
			f.Symbol, f.ID, f.Remaining); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

func (s *pgStore) ReportCancel(ctx context.Context, symbol string, id uint64) error {
	_, err := s.pool.Exec(ctx,
		`DELETE FROM resting WHERE symbol = $1 AND id = $2`, symbol, id)
	return err
}

func (s *pgStore) ReportTrade(ctx context.Context, t storage.Trade) error {
	_, err := s.pool.Exec(ctx, `
		INSERT INTO trades (symbol, taker, maker, price, qty)
		VALUES ($1, $2, $3, $4, $5)`,
		t.Symbol, t.Taker, t.Maker, t.Price, t.Qty)
	return err
}

func (s *pgStore) Commit(ctx context.Context) error {
	// Each write is applied in its own transaction, so nothing is buffered;
	// the runtime's watermark (highest applied write) is already durable.
	return nil
}

func (s *pgStore) PullPosition(ctx context.Context, user, symbol string) (int64, error) {
	var net int64
	err := s.pool.QueryRow(ctx,
		`SELECT net FROM positions WHERE owner = $1 AND symbol = $2`,
		user, symbol).Scan(&net)
	if err != nil {
		if err.Error() == "no rows in result set" {
			return 0, nil
		}
		return 0, nil // unknown account -> flat, not an error
	}
	return net, nil
}

func (s *pgStore) PullLevels(ctx context.Context, symbol, side string, fromPrice int64, count int) ([]storage.RestingOrder, error) {
	// Take the best `count` price levels beyond fromPrice, then all their
	// orders in seq order. Buy pulls prices below fromPrice (best = highest);
	// sell pulls prices above (best = lowest).
	var q string
	if side == "buy" {
		q = `
			WITH lvls AS (
			  SELECT DISTINCT price FROM resting
			  WHERE symbol = $1 AND side = 'buy' AND price < $2
			  ORDER BY price DESC LIMIT $3)
			SELECT r.id, r.price, r.leaves, r.seq
			FROM resting r JOIN lvls ON r.price = lvls.price
			WHERE r.symbol = $1 AND r.side = 'buy'
			ORDER BY r.price DESC, r.seq ASC`
	} else {
		q = `
			WITH lvls AS (
			  SELECT DISTINCT price FROM resting
			  WHERE symbol = $1 AND side = 'sell' AND price > $2
			  ORDER BY price ASC LIMIT $3)
			SELECT r.id, r.price, r.leaves, r.seq
			FROM resting r JOIN lvls ON r.price = lvls.price
			WHERE r.symbol = $1 AND r.side = 'sell'
			ORDER BY r.price ASC, r.seq ASC`
	}
	rows, err := s.pool.Query(ctx, q, symbol, fromPrice, count)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []storage.RestingOrder
	for rows.Next() {
		o := storage.RestingOrder{Symbol: symbol, Side: side}
		if err := rows.Scan(&o.ID, &o.Price, &o.Leaves, &o.Seq); err != nil {
			return nil, err
		}
		out = append(out, o)
	}
	return out, rows.Err()
}

func (s *pgStore) PullWatermarks(ctx context.Context) (uint64, uint64, error) {
	var maxID, maxRank int64
	if err := s.pool.QueryRow(ctx,
		`SELECT COALESCE(MAX(id), 0) FROM orders`).Scan(&maxID); err != nil {
		return 0, 0, err
	}
	if err := s.pool.QueryRow(ctx,
		`SELECT COALESCE(MAX(seq), 0) FROM resting`).Scan(&maxRank); err != nil {
		return 0, 0, err
	}
	return uint64(maxID), uint64(maxRank), nil
}

func (s *pgStore) Close() error {
	s.pool.Close()
	return nil
}
