package storage

import (
	"bytes"
	"context"
	"sync"
	"testing"

	"github.com/garana/codicis/helpers/internal/wire"
)

// fakeStore records calls and serves canned reads.
type fakeStore struct {
	mu      sync.Mutex
	orders  int
	rests   int
	fills   int
	cancels int
	trades  int
	pos     map[string]int64
	levels  []RestingOrder
	maxID   uint64
	maxRank uint64
}

func (f *fakeStore) ReportOrder(_ context.Context, _ Order) error {
	f.mu.Lock()
	f.orders++
	f.mu.Unlock()
	return nil
}
func (f *fakeStore) ReportRest(_ context.Context, _ RestingOrder) error {
	f.mu.Lock()
	f.rests++
	f.mu.Unlock()
	return nil
}
func (f *fakeStore) ReportFill(_ context.Context, _ Fill) error {
	f.mu.Lock()
	f.fills++
	f.mu.Unlock()
	return nil
}
func (f *fakeStore) ReportCancel(_ context.Context, _ string, _ uint64) error {
	f.mu.Lock()
	f.cancels++
	f.mu.Unlock()
	return nil
}
func (f *fakeStore) ReportTrade(_ context.Context, _ Trade) error {
	f.mu.Lock()
	f.trades++
	f.mu.Unlock()
	return nil
}
func (f *fakeStore) Commit(_ context.Context) error { return nil }
func (f *fakeStore) PullPosition(_ context.Context, user, symbol string) (int64, error) {
	return f.pos[user+"/"+symbol], nil
}
func (f *fakeStore) PullLevels(_ context.Context, _, _ string, _ int64, _ int) ([]RestingOrder, error) {
	return f.levels, nil
}
func (f *fakeStore) PullWatermarks(_ context.Context) (uint64, uint64, error) {
	return f.maxID, f.maxRank, nil
}
func (f *fakeStore) Close() error { return nil }

func decodeAll(t *testing.T, b []byte) map[uint64]*wire.Message {
	t.Helper()
	out := map[uint64]*wire.Message{}
	r := wire.NewReader(bytes.NewReader(b))
	for {
		m, err := r.ReadMessage()
		if err != nil {
			break
		}
		out[m.ReqID] = m
	}
	return out
}

func TestServerDispatch(t *testing.T) {
	f := &fakeStore{
		pos: map[string]int64{"u1/BTC": 42},
		levels: []RestingOrder{
			{ID: 1, Price: 100, Leaves: 5, Seq: 1},
			{ID: 2, Price: 100, Leaves: 3, Seq: 2},
			{ID: 3, Price: 99, Leaves: 7, Seq: 3},
		},
		maxID:   100,
		maxRank: 900,
	}

	var in bytes.Buffer
	write := func(m *wire.Message) { in.Write(wire.Encode(m)) }

	o := &wire.Message{ReqID: 1, Type: "report_order"}
	o.Set("symbol", "BTC")
	o.Set("side", "buy")
	o.SetInt("id", 1)
	o.SetInt("price", 100)
	o.SetInt("qty", 5)
	write(o)

	c := &wire.Message{ReqID: 2, Type: "commit"}
	write(c)

	pp := &wire.Message{ReqID: 3, Type: "pull_position"}
	pp.Set("user", "u1")
	pp.Set("symbol", "BTC")
	write(pp)

	pl := &wire.Message{ReqID: 4, Type: "pull_levels"}
	pl.Set("symbol", "BTC")
	pl.Set("side", "buy")
	pl.SetInt("from_price", 1000000)
	pl.SetInt("count", 10)
	write(pl)

	wm := &wire.Message{ReqID: 5, Type: "pull_watermarks"}
	write(wm)

	var out bytes.Buffer
	srv := NewServer(f, &in, &out, 4)
	if err := srv.Run(context.Background()); err != nil {
		t.Fatalf("run: %v", err)
	}

	resp := decodeAll(t, out.Bytes())
	if s, _ := resp[1].Get("status"); s != "ok" {
		t.Fatalf("report_order status: %q", s)
	}
	if f.orders != 1 {
		t.Fatalf("orders=%d", f.orders)
	}
	// commit echoes the highest write req_id (the order at 1).
	if w, _ := resp[2].Get("committed"); w != "1" {
		t.Fatalf("committed: %q", w)
	}
	if n, _ := resp[3].Get("net"); n != "42" {
		t.Fatalf("net: %q", n)
	}
	blob, _ := resp[4].Get("orders")
	if blob != "1,100,5,1;2,100,3,2;3,99,7,3;" {
		t.Fatalf("orders blob: %q", blob)
	}
	if lv, _ := resp[4].Get("count"); lv != "2" { // two price levels: 100, 99
		t.Fatalf("levels: %q", lv)
	}
	if v, _ := resp[5].Get("max_id"); v != "100" {
		t.Fatalf("max_id: %q", v)
	}
	if v, _ := resp[5].Get("max_rank"); v != "900" {
		t.Fatalf("max_rank: %q", v)
	}
}
