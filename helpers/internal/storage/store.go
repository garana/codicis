// Package storage provides the shared storage-helper runtime: it speaks the
// codicis helper wire protocol on stdin/stdout and dispatches each request to a
// backend Store. A concrete helper binary supplies only a Store implementation
// (a thin adapter over its database driver), so all the protocol handling lives
// here once and each binary links just its own driver.
package storage

import "context"

// Order is a reported new order (report_order).
type Order struct {
	ReqID  uint64
	Symbol string
	Owner  string // owning user UUID ("" = anonymous)
	ID     uint64 // internal order id (unique within a symbol)
	Side   string // "buy" or "sell"
	Price  int64
	Qty    int64
}

// RestingOrder is an order resting in the continuous book (report_rest /
// pull_levels).
type RestingOrder struct {
	ReqID  uint64
	Symbol string
	Side   string
	ID     uint64
	Price  int64
	Leaves int64
	Seq    uint64 // arrival sequence (time priority)
}

// Fill is a (partial or complete) execution of one order (report_fill).
type Fill struct {
	ReqID     uint64
	Symbol    string
	ID        uint64
	Qty       int64
	Remaining int64
	Complete  bool // true when the order is fully filled
}

// Trade is one execution between a taker and a maker (report_trade).
type Trade struct {
	ReqID  uint64
	Symbol string
	Taker  uint64
	Maker  uint64
	Price  int64
	Qty    int64
}

// Store is the system-of-record behind a storage helper. Writes
// (Report*/Commit) form the strictly-ordered committer stream; reads
// (Pull*) may run concurrently and complete out of order.
//
// Every method must be safe to call from multiple goroutines: the runtime
// serializes writes but may dispatch reads concurrently.
type Store interface {
	// ReportOrder records a newly accepted order (its owner/side let later
	// fills be attributed to an account's position).
	ReportOrder(ctx context.Context, o Order) error

	// ReportRest records that an order now rests in the continuous book.
	ReportRest(ctx context.Context, r RestingOrder) error

	// ReportFill applies a fill: it decrements the resting order (removing it
	// when fully filled) and updates the owner's net position for the symbol.
	ReportFill(ctx context.Context, f Fill) error

	// ReportCancel removes a resting order from the book.
	ReportCancel(ctx context.Context, symbol string, id uint64) error

	// ReportTrade records an anonymous trade print.
	ReportTrade(ctx context.Context, t Trade) error

	// Commit durably flushes everything reported so far. The runtime replies
	// with the highest write req_id, which is durable once Commit returns.
	Commit(ctx context.Context) error

	// PullPosition returns the owner's net position for a symbol (buy +,
	// sell -; 0 if unknown).
	PullPosition(ctx context.Context, user, symbol string) (int64, error)

	// PullLevels returns the resting orders on side beyond fromPrice (worse
	// than it), covering at most count price levels, best price first and
	// arrival (seq) order within a level.
	PullLevels(ctx context.Context, symbol, side string, fromPrice int64, count int) ([]RestingOrder, error)

	// PullWatermarks returns the largest order id ever reported and the largest
	// resting priority rank, so the engine can seed its id/priority counters
	// above them on boot (avoiding id collisions and priority inversion after a
	// restart).
	PullWatermarks(ctx context.Context) (maxID, maxRank uint64, err error)

	// Close releases the backend (connections, pools).
	Close() error
}
