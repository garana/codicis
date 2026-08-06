// Command storage-mysql is a reference codicis storage helper backed by MySQL.
// It speaks the storage-helper wire protocol on stdin/stdout and persists to
// the schema in helpers/schema/mysql.sql.
//
// Config (flags override env):
//
//	-dsn / $CODICIS_MYSQL_DSN   MySQL DSN, e.g.
//	                            codicis:codicis@tcp(localhost:3306)/codicis
//	-migrate                    apply the schema on startup (idempotent)
//	-read-concurrency           max concurrent Pull* reads (default 8)
package main

import (
	"context"
	"database/sql"
	"flag"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	_ "github.com/go-sql-driver/mysql"

	"github.com/garana/codicis/helpers/internal/storage"
	"github.com/garana/codicis/helpers/schema"
)

func main() {
	dsn := flag.String("dsn", os.Getenv("CODICIS_MYSQL_DSN"), "MySQL DSN")
	migrate := flag.Bool("migrate", false, "apply the schema on startup")
	readConc := flag.Int("read-concurrency", 8, "max concurrent Pull* reads")
	flag.Parse()
	if *dsn == "" {
		log.Fatal("storage-mysql: -dsn or $CODICIS_MYSQL_DSN is required")
	}

	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	db, err := sql.Open("mysql", *dsn)
	if err != nil {
		log.Fatalf("storage-mysql: open: %v", err)
	}
	defer db.Close()
	if err := db.PingContext(ctx); err != nil {
		log.Fatalf("storage-mysql: ping: %v", err)
	}
	if *migrate {
		if err := applySchema(ctx, db, schema.MySQL); err != nil {
			log.Fatalf("storage-mysql: migrate: %v", err)
		}
	}

	store := &myStore{db: db}
	srv := storage.NewServer(store, os.Stdin, os.Stdout, *readConc)
	if err := srv.Run(ctx); err != nil {
		log.Fatalf("storage-mysql: %v", err)
	}
}

// applySchema runs each ';'-terminated statement (the driver executes one at a
// time). Comment lines ("--") are stripped first so a leading comment block is
// not sent as SQL.
func applySchema(ctx context.Context, db *sql.DB, ddl string) error {
	var sb strings.Builder
	for _, line := range strings.Split(ddl, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "--") {
			continue
		}
		sb.WriteString(line)
		sb.WriteByte('\n')
	}
	for _, stmt := range strings.Split(sb.String(), ";") {
		if strings.TrimSpace(stmt) == "" {
			continue
		}
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			return err
		}
	}
	return nil
}

// myStore implements storage.Store over database/sql + the MySQL driver.
type myStore struct {
	db *sql.DB
}

func (s *myStore) ReportOrder(ctx context.Context, o storage.Order) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO orders (symbol, id, owner, side, price, qty)
		VALUES (?, ?, ?, ?, ?, ?)
		ON DUPLICATE KEY UPDATE
			owner = VALUES(owner), side = VALUES(side),
			price = VALUES(price), qty = VALUES(qty)`,
		o.Symbol, o.ID, o.Owner, o.Side, o.Price, o.Qty)
	return err
}

func (s *myStore) ReportRest(ctx context.Context, r storage.RestingOrder) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO resting (symbol, id, side, price, leaves, seq)
		VALUES (?, ?, ?, ?, ?, ?)
		ON DUPLICATE KEY UPDATE
			side = VALUES(side), price = VALUES(price),
			leaves = VALUES(leaves), seq = VALUES(seq)`,
		r.Symbol, r.ID, r.Side, r.Price, r.Leaves, r.Seq)
	return err
}

func (s *myStore) ReportFill(ctx context.Context, f storage.Fill) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after a successful commit

	if _, err = tx.ExecContext(ctx, `
		INSERT INTO fills (symbol, id, qty, remaining, complete)
		VALUES (?, ?, ?, ?, ?)`,
		f.Symbol, f.ID, f.Qty, f.Remaining, f.Complete); err != nil {
		return err
	}
	// Look up the order's owner/side to attribute the position delta.
	var owner, side string
	err = tx.QueryRowContext(ctx,
		`SELECT owner, side FROM orders WHERE symbol = ? AND id = ?`,
		f.Symbol, f.ID).Scan(&owner, &side)
	if err == nil && owner != "" {
		delta := f.Qty
		if side == "sell" {
			delta = -f.Qty
		}
		if _, err = tx.ExecContext(ctx, `
			INSERT INTO positions (owner, symbol, net) VALUES (?, ?, ?)
			ON DUPLICATE KEY UPDATE net = net + VALUES(net)`,
			owner, f.Symbol, delta); err != nil {
			return err
		}
	} else if err != nil && err != sql.ErrNoRows {
		return err
	}
	if f.Complete {
		_, err = tx.ExecContext(ctx,
			`DELETE FROM resting WHERE symbol = ? AND id = ?`, f.Symbol, f.ID)
	} else {
		_, err = tx.ExecContext(ctx,
			`UPDATE resting SET leaves = ? WHERE symbol = ? AND id = ?`,
			f.Remaining, f.Symbol, f.ID)
	}
	if err != nil {
		return err
	}
	return tx.Commit()
}

func (s *myStore) ReportCancel(ctx context.Context, symbol string, id uint64) error {
	_, err := s.db.ExecContext(ctx,
		`DELETE FROM resting WHERE symbol = ? AND id = ?`, symbol, id)
	return err
}

func (s *myStore) ReportTrade(ctx context.Context, t storage.Trade) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO trades (symbol, taker, maker, price, qty)
		VALUES (?, ?, ?, ?, ?)`,
		t.Symbol, t.Taker, t.Maker, t.Price, t.Qty)
	return err
}

func (s *myStore) Commit(_ context.Context) error { return nil }

func (s *myStore) PullPosition(ctx context.Context, user, symbol string) (int64, error) {
	var net int64
	err := s.db.QueryRowContext(ctx,
		`SELECT net FROM positions WHERE owner = ? AND symbol = ?`,
		user, symbol).Scan(&net)
	if err == sql.ErrNoRows {
		return 0, nil
	}
	if err != nil {
		return 0, err
	}
	return net, nil
}

func (s *myStore) PullLevels(ctx context.Context, symbol, side string, fromPrice int64, count int) ([]storage.RestingOrder, error) {
	var q string
	if side == "buy" {
		q = `
			WITH lvls AS (
			  SELECT DISTINCT price FROM resting
			  WHERE symbol = ? AND side = 'buy' AND price < ?
			  ORDER BY price DESC LIMIT ?)
			SELECT r.id, r.price, r.leaves, r.seq
			FROM resting r JOIN lvls ON r.price = lvls.price
			WHERE r.symbol = ? AND r.side = 'buy'
			ORDER BY r.price DESC, r.seq ASC`
	} else {
		q = `
			WITH lvls AS (
			  SELECT DISTINCT price FROM resting
			  WHERE symbol = ? AND side = 'sell' AND price > ?
			  ORDER BY price ASC LIMIT ?)
			SELECT r.id, r.price, r.leaves, r.seq
			FROM resting r JOIN lvls ON r.price = lvls.price
			WHERE r.symbol = ? AND r.side = 'sell'
			ORDER BY r.price ASC, r.seq ASC`
	}
	rows, err := s.db.QueryContext(ctx, q, symbol, fromPrice, count, symbol)
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

func (s *myStore) Close() error { return s.db.Close() }
