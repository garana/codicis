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

# Full integration: bring up every backing service (docker-compose), run all
# helper integration tests against them, then tear down. This is what the
# clean-room invokes.
integration: toolchain ## docker up -> run all integration tests -> docker down
	GO=$(GO) ./docker/run-integration.sh

# Individual integration tests expect a running service; point the env at it.
integration-postgres: toolchain
	$(GO) test -tags integration ./cmd/storage-postgres/
integration-mysql: toolchain
	$(GO) test -tags integration ./cmd/storage-mysql/
integration-mongo: toolchain
	$(GO) test -tags integration ./cmd/storage-mongo/

vendor: toolchain ## re-resolve and vendor dependencies into ./vendor
	GOFLAGS=-mod=mod $(GO) mod tidy
	GOFLAGS=-mod=mod $(GO) mod vendor

clean:
	rm -rf bin
