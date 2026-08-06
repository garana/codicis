package wire

import (
	"bytes"
	"io"
	"strings"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	m := &Message{ReqID: 7, Type: "report_order"}
	m.Set("symbol", "BTC")
	m.Set("owner", "u1")
	m.SetInt("price", 100)
	m.SetInt("qty", 10)

	enc := Encode(m)
	r := NewReader(bytes.NewReader(enc))
	got, err := r.ReadMessage()
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.ReqID != 7 || got.Type != "report_order" {
		t.Fatalf("envelope: %+v", got)
	}
	if v, _ := got.Get("symbol"); v != "BTC" {
		t.Fatalf("symbol: %q", v)
	}
	if got.GetInt("price") != 100 || got.GetInt("qty") != 10 {
		t.Fatalf("ints: %+v", got.Fields)
	}
}

func TestPipelined(t *testing.T) {
	a := &Message{ReqID: 1, Type: "ping"}
	b := &Message{ReqID: 2, Type: "commit"}
	buf := append(Encode(a), Encode(b)...)

	r := NewReader(bytes.NewReader(buf))
	m1, err := r.ReadMessage()
	if err != nil {
		t.Fatalf("m1: %v", err)
	}
	m2, err := r.ReadMessage()
	if err != nil {
		t.Fatalf("m2: %v", err)
	}
	if m1.ReqID != 1 || m2.ReqID != 2 {
		t.Fatalf("ids: %d %d", m1.ReqID, m2.ReqID)
	}
	if _, err := r.ReadMessage(); err != io.EOF {
		t.Fatalf("want EOF, got %v", err)
	}
}

func TestCRLFTolerated(t *testing.T) {
	raw := "req_id=3\r\ntype=ping\r\n\r\n"
	r := NewReader(strings.NewReader(raw))
	m, err := r.ReadMessage()
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if m.ReqID != 3 || m.Type != "ping" {
		t.Fatalf("got %+v", m)
	}
}

func TestBadReqID(t *testing.T) {
	raw := "req_id=notanumber\ntype=ping\n\n"
	r := NewReader(strings.NewReader(raw))
	if _, err := r.ReadMessage(); err == nil {
		t.Fatal("want error on bad req_id")
	}
}
