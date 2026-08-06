// Package wire implements the codicis helper text codec: the "key=value"
// line protocol codicis speaks to its child-process helpers over stdin/stdout.
//
// A message is a req_id, a type, and an ordered list of key/value fields:
//
//	req_id=<uint64>\n
//	type=<string>\n
//	<key>=<value>\n        (zero or more)
//	\n                     (blank line terminates the record)
//
// req_id and type are reserved keys; a trailing '\r' on any line is tolerated
// (CRLF endings). Requests are pipelined: several may be in flight and a helper
// may answer out of order, since each response echoes the request's req_id.
//
// This is a faithful reimplementation of the C++ TextHelperCodec so that Go
// helpers are drop-in compatible with codicis's reference C++ helpers.
package wire

import (
	"bufio"
	"errors"
	"io"
	"strconv"
	"strings"
)

// Message is one decoded helper record.
type Message struct {
	ReqID  uint64
	Type   string
	Fields []Field // ordered, may contain duplicate keys
}

// Field is a single key/value pair.
type Field struct {
	Key   string
	Value string
}

// Get returns the first value for key and whether it was present.
func (m *Message) Get(key string) (string, bool) {
	for _, f := range m.Fields {
		if f.Key == key {
			return f.Value, true
		}
	}
	return "", false
}

// GetInt returns the first value for key parsed as a signed integer (0 if
// absent or unparseable).
func (m *Message) GetInt(key string) int64 {
	if v, ok := m.Get(key); ok {
		n, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		if err == nil {
			return n
		}
	}
	return 0
}

// Set appends a field.
func (m *Message) Set(key, value string) {
	m.Fields = append(m.Fields, Field{Key: key, Value: value})
}

// SetInt appends a field with a decimal integer value.
func (m *Message) SetInt(key string, v int64) {
	m.Set(key, strconv.FormatInt(v, 10))
}

// maxRecord caps a single record so a peer that never terminates a record
// cannot exhaust memory (mirrors the C++ codec's 64 MiB guard).
const maxRecord = 64 * 1024 * 1024

// ErrTooLarge is returned when a record exceeds maxRecord without terminating.
var ErrTooLarge = errors.New("wire: record exceeds cap without terminator")

// Reader decodes messages from a stream.
type Reader struct {
	br *bufio.Reader
}

// NewReader wraps r.
func NewReader(r io.Reader) *Reader {
	return &Reader{br: bufio.NewReaderSize(r, 64*1024)}
}

// ReadMessage decodes the next record. It returns io.EOF at a clean end of
// stream (no partial record buffered).
func (r *Reader) ReadMessage() (*Message, error) {
	m := &Message{}
	haveReqID := false
	total := 0
	sawAny := false
	for {
		line, err := r.br.ReadString('\n')
		total += len(line)
		if total > maxRecord {
			return nil, ErrTooLarge
		}
		if err != nil {
			if err == io.EOF {
				if !sawAny && line == "" {
					return nil, io.EOF // clean end between records
				}
				return nil, io.ErrUnexpectedEOF
			}
			return nil, err
		}
		sawAny = true
		line = strings.TrimSuffix(line, "\n")
		line = strings.TrimSuffix(line, "\r")
		if line == "" {
			break // blank line terminates the record
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return nil, errors.New("wire: line without '='")
		}
		key, val := line[:eq], line[eq+1:]
		switch key {
		case "req_id":
			n, perr := strconv.ParseUint(val, 10, 64)
			if perr != nil {
				return nil, errors.New("wire: bad req_id")
			}
			m.ReqID = n
			haveReqID = true
		case "type":
			m.Type = val
		default:
			m.Set(key, val)
		}
	}
	if !haveReqID {
		return nil, errors.New("wire: missing req_id")
	}
	return m, nil
}

// Encode writes m to a byte slice in the text framing.
func Encode(m *Message) []byte {
	var b strings.Builder
	b.WriteString("req_id=")
	b.WriteString(strconv.FormatUint(m.ReqID, 10))
	b.WriteByte('\n')
	b.WriteString("type=")
	b.WriteString(m.Type)
	b.WriteByte('\n')
	for _, f := range m.Fields {
		b.WriteString(f.Key)
		b.WriteByte('=')
		b.WriteString(f.Value)
		b.WriteByte('\n')
	}
	b.WriteByte('\n') // blank line terminates
	return []byte(b.String())
}
