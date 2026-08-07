#!/bin/sh
# Run the helpers/integration black-box tests.
#
# Bootstraps the tree-local pinned Go (no global Go), builds the helper binaries
# the tests exec, then runs the vendored testcontainers-go tests. testcontainers
# brings up each backing service on an ephemeral, Docker-assigned port -- no
# docker-compose and no fixed host ports, so nothing collides with a host
# postgres/mongod/etc.
#
# Requirements: a Docker daemon (for the service containers) and curl/tar/sha256
# (one-time Go SDK bootstrap). Extra args are passed through to `go test`.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)   # helpers/integration
HELPERS=$(cd "$HERE/.." && pwd)       # helpers

"$HELPERS/tools/bootstrap-go.sh"
GO="$HELPERS/.toolchain/go/bin/go"
export GOROOT="$HELPERS/.toolchain/go"
export GOTOOLCHAIN=local

# Build the helper binaries the black-box tests os/exec.
(cd "$HELPERS" && "$GO" build -mod=vendor -o bin/storage-postgres ./cmd/storage-postgres)
export CODICIS_STORAGE_POSTGRES_BIN="$HELPERS/bin/storage-postgres"

cd "$HERE"
# vendor/ is gitignored to keep the repo lean; populate it once (needs network)
# then build fully offline from it.
if [ ! -d vendor ]; then
  echo "run.sh: vendoring integration deps (one-time)"
  GOFLAGS=-mod=mod "$GO" mod vendor
fi
exec "$GO" test -tags integration -mod=vendor -count=1 "$@" ./...
