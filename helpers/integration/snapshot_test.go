//go:build integration

package integration

import (
	"os"
	"path/filepath"
	"testing"
)

// snapshot compares got against testdata/<name>.golden. With UPDATE_SNAPSHOTS=1
// (or when the golden does not yet exist) it (re)writes the golden instead --
// review the diff before committing a re-blessed snapshot.
func snapshot(t *testing.T, name string, got []byte) {
	t.Helper()
	path := filepath.Join("testdata", name+".golden")
	if os.Getenv("UPDATE_SNAPSHOTS") == "1" {
		writeGolden(t, path, got)
		return
	}
	want, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		writeGolden(t, path, got)
		t.Logf("wrote new golden %s (review + commit it)", path)
		return
	}
	if err != nil {
		t.Fatalf("read golden %s: %v", path, err)
	}
	if string(got) != string(want) {
		t.Fatalf("snapshot %s mismatch (UPDATE_SNAPSHOTS=1 to re-bless)\n"+
			"--- want ---\n%s\n--- got ---\n%s", name, want, got)
	}
}

func writeGolden(t *testing.T, path string, got []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, got, 0o644); err != nil {
		t.Fatal(err)
	}
}
