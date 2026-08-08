# codicis data helpers (Go). Fully self-contained: the Go toolchain is pinned
# and fetched into a repo-local, gitignored ./.toolchain (never the globally
# installed Go), and library deps are vendored under ./vendor (never the global
# module cache). So a host needs only curl/tar/sha256 to bootstrap -- no
# pre-existing Go -- and every build is reproducible and offline after that.

# Tree-local, pinned Go (see tools/bootstrap-go.sh); GOROOT points at it so the
# global Go and any global GOROOT are never consulted. GOTOOLCHAIN=local because
# the local Go is already the pinned version (no re-exec / download).
GO         := $(CURDIR)/.toolchain/go/bin/go
export GOROOT     := $(CURDIR)/.toolchain/go
export GOMODCACHE := $(CURDIR)/.gocache
export GOFLAGS    := -mod=vendor
export GOTOOLCHAIN := local

# Each backend is its own binary linking only its own driver.
STORAGE := storage-postgres storage-mysql storage-mongo
FEED    := feed-redis feed-nats feed-kafka feed-rabbitmq feed-mqtt \
           feed-zeromq feed-sns feed-sqs
HELPERS := $(STORAGE) $(FEED)

.PHONY: all toolchain build test integration vendor tidy clean

all: build

toolchain: ## fetch the pinned Go SDK into ./toolchain (idempotent)
	./tools/bootstrap-go.sh

build: toolchain ## build every helper into ./bin
	@mkdir -p bin
	@for c in $(HELPERS); do \
	  echo "build $$c"; \
	  $(GO) build -o bin/$$c ./cmd/$$c/ || exit 1; \
	done

test: toolchain ## run the unit tests (no external services needed)
	$(GO) test ./internal/...

# Full integration: the black-box suite in ./integration. testcontainers-go
# brings up each backing service on an ephemeral Docker-assigned port (no
# docker-compose, no fixed host ports, nothing to collide with). run.sh
# bootstraps the tree-local Go, builds the helper binaries the tests exec, and
# runs the vendored suite. This is what the clean-room invokes; it needs only a
# Docker daemon + curl/tar/sha256. Extra args pass through to `go test`.
integration: ## run the black-box integration suite (testcontainers-go)
	./integration/run.sh

vendor: toolchain ## re-resolve and vendor dependencies into ./vendor
	GOFLAGS=-mod=mod $(GO) mod tidy
	GOFLAGS=-mod=mod $(GO) mod vendor

clean:
	rm -rf bin
