package storage

import (
	"context"
	"fmt"
	"io"
	"strconv"
	"strings"
	"sync"

	"github.com/garana/codicis/helpers/internal/wire"
)

// Server runs the storage-helper protocol loop over a reader/writer pair
// (codicis's request stream and the response stream) against a Store.
type Server struct {
	store       Store
	r           *wire.Reader
	w           io.Writer
	wmu         sync.Mutex // serializes framed writes to w
	readSem     chan struct{}
	highWriteMu sync.Mutex
	highWrite   uint64 // highest req_id of an applied write op
}

// NewServer builds a server. readConcurrency bounds the number of concurrent
// Pull* reads (>=1); writes are always applied in arrival order.
func NewServer(store Store, r io.Reader, w io.Writer, readConcurrency int) *Server {
	if readConcurrency < 1 {
		readConcurrency = 1
	}
	return &Server{
		store:   store,
		r:       wire.NewReader(r),
		w:       w,
		readSem: make(chan struct{}, readConcurrency),
	}
}

// Run processes requests until the input stream closes. It returns nil on a
// clean EOF.
func (s *Server) Run(ctx context.Context) error {
	var wg sync.WaitGroup
	for {
		msg, err := s.r.ReadMessage()
		if err == io.EOF {
			break
		}
		if err != nil {
			wg.Wait()
			return err
		}
		if isRead(msg.Type) {
			// Reads may run concurrently and reply out of order.
			s.readSem <- struct{}{}
			wg.Add(1)
			go func(m *wire.Message) {
				defer wg.Done()
				defer func() { <-s.readSem }()
				s.handleRead(ctx, m)
			}(msg)
			continue
		}
		// Writes are applied in strict arrival order.
		s.handleWrite(ctx, msg)
	}
	wg.Wait()
	return nil
}

func isRead(t string) bool {
	return t == "pull_levels" || t == "pull_position" ||
		t == "pull_watermarks" || t == "ping"
}

func (s *Server) noteWrite(reqID uint64) {
	s.highWriteMu.Lock()
	if reqID > s.highWrite {
		s.highWrite = reqID
	}
	s.highWriteMu.Unlock()
}

func (s *Server) watermark() uint64 {
	s.highWriteMu.Lock()
	defer s.highWriteMu.Unlock()
	return s.highWrite
}

func (s *Server) reply(m *wire.Message) {
	s.wmu.Lock()
	_, _ = s.w.Write(wire.Encode(m))
	s.wmu.Unlock()
}

func (s *Server) ackOK(reqID uint64, typ string) {
	resp := &wire.Message{ReqID: reqID, Type: typ}
	resp.Set("status", "ok")
	s.reply(resp)
}

func (s *Server) ackErr(reqID uint64, typ, reason string) {
	resp := &wire.Message{ReqID: reqID, Type: typ}
	resp.Set("status", "error")
	resp.Set("reason", reason)
	s.reply(resp)
}

func (s *Server) handleWrite(ctx context.Context, m *wire.Message) {
	switch m.Type {
	case "report_order":
		o := Order{
			ReqID: m.ReqID, ID: uint64(m.GetInt("id")),
			Price: m.GetInt("price"), Qty: m.GetInt("qty"),
		}
		o.Symbol, _ = m.Get("symbol")
		o.Owner, _ = m.Get("owner")
		o.Side, _ = m.Get("side")
		s.noteWrite(m.ReqID)
		if err := s.store.ReportOrder(ctx, o); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		s.ackOK(m.ReqID, m.Type)
	case "report_rest":
		r := RestingOrder{
			ReqID: m.ReqID, ID: uint64(m.GetInt("id")),
			Price: m.GetInt("price"), Leaves: m.GetInt("leaves"),
			Seq: uint64(m.GetInt("seq")),
		}
		r.Symbol, _ = m.Get("symbol")
		r.Side, _ = m.Get("side")
		s.noteWrite(m.ReqID)
		if err := s.store.ReportRest(ctx, r); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		s.ackOK(m.ReqID, m.Type)
	case "report_fill":
		f := Fill{
			ReqID: m.ReqID, ID: uint64(m.GetInt("id")),
			Qty: m.GetInt("qty"), Remaining: m.GetInt("remaining"),
		}
		f.Symbol, _ = m.Get("symbol")
		if st, _ := m.Get("status"); st == "filled" {
			f.Complete = true
		}
		s.noteWrite(m.ReqID)
		if err := s.store.ReportFill(ctx, f); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		s.ackOK(m.ReqID, m.Type)
	case "report_cancel":
		symbol, _ := m.Get("symbol")
		s.noteWrite(m.ReqID)
		if err := s.store.ReportCancel(ctx, symbol, uint64(m.GetInt("id"))); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		s.ackOK(m.ReqID, m.Type)
	case "report_trade":
		t := Trade{
			ReqID: m.ReqID, Taker: uint64(m.GetInt("taker")),
			Maker: uint64(m.GetInt("maker")), Price: m.GetInt("price"),
			Qty: m.GetInt("qty"),
		}
		t.Symbol, _ = m.Get("symbol")
		s.noteWrite(m.ReqID)
		if err := s.store.ReportTrade(ctx, t); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		s.ackOK(m.ReqID, m.Type)
	case "commit":
		if err := s.store.Commit(ctx); err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		resp := &wire.Message{ReqID: m.ReqID, Type: "commit"}
		resp.Set("committed", strconv.FormatUint(s.watermark(), 10))
		s.reply(resp)
	default:
		s.ackErr(m.ReqID, m.Type, "unknown type: "+m.Type)
	}
}

func (s *Server) handleRead(ctx context.Context, m *wire.Message) {
	switch m.Type {
	case "ping":
		resp := &wire.Message{ReqID: m.ReqID, Type: "pong"}
		s.reply(resp)
	case "pull_position":
		user, _ := m.Get("user")
		symbol, _ := m.Get("symbol")
		net, err := s.store.PullPosition(ctx, user, symbol)
		if err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		resp := &wire.Message{ReqID: m.ReqID, Type: m.Type}
		resp.SetInt("net", net)
		s.reply(resp)
	case "pull_watermarks":
		maxID, maxRank, err := s.store.PullWatermarks(ctx)
		if err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		resp := &wire.Message{ReqID: m.ReqID, Type: m.Type}
		resp.Set("max_id", strconv.FormatUint(maxID, 10))
		resp.Set("max_rank", strconv.FormatUint(maxRank, 10))
		s.reply(resp)
	case "pull_levels":
		symbol, _ := m.Get("symbol")
		side, _ := m.Get("side")
		from := m.GetInt("from_price")
		count := int(m.GetInt("count"))
		orders, err := s.store.PullLevels(ctx, symbol, side, from, count)
		if err != nil {
			s.ackErr(m.ReqID, m.Type, err.Error())
			return
		}
		resp := &wire.Message{ReqID: m.ReqID, Type: m.Type}
		resp.Set("symbol", symbol)
		resp.Set("side", side)
		resp.Set("orders", EncodeOrdersBlob(orders))
		resp.Set("count", strconv.Itoa(countLevels(orders)))
		s.reply(resp)
	default:
		s.ackErr(m.ReqID, m.Type, "unknown type: "+m.Type)
	}
}

// EncodeOrdersBlob renders resting orders as codicis's pull_levels blob:
// records "id,price,leaves,seq" joined by ';' (best price first, seq order
// within a level -- the caller must supply them already ordered).
func EncodeOrdersBlob(orders []RestingOrder) string {
	var b strings.Builder
	for _, o := range orders {
		fmt.Fprintf(&b, "%d,%d,%d,%d;", o.ID, o.Price, o.Leaves, o.Seq)
	}
	return b.String()
}

// countLevels counts the distinct price levels present in orders.
func countLevels(orders []RestingOrder) int {
	n := 0
	var last int64
	first := true
	for _, o := range orders {
		if first || o.Price != last {
			n++
			last = o.Price
			first = false
		}
	}
	return n
}
