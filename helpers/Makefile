# codicis data helpers (Go). Dependencies are vendored under vendor/ and, when
# fetching, cached in a repo-local .gocache (never the global module cache), so
# builds are reproducible and self-contained.

export GOMODCACHE := $(CURDIR)/.gocache
export GOFLAGS    := -mod=vendor
export GOTOOLCHAIN := local

# Each backend is its own binary linking only its own driver.
STORAGE := storage-postgres storage-mysql storage-mongo

.PHONY: all build test vendor tidy clean $(addprefix build-,$(STORAGE))

all: build

build: ## build every helper into ./bin
	@mkdir -p bin
	@for c in $(STORAGE); do \
	  echo "build $$c"; \
	  go build -o bin/$$c ./cmd/$$c/ || exit 1; \
	done

test: ## run the unit tests (no external services needed)
	go test ./internal/...

# Integration tests need a running backing service; point the DSN at it.
integration-postgres:
	CODICIS_PG_DSN?=; go test -tags integration ./cmd/storage-postgres/
integration-mysql:
	go test -tags integration ./cmd/storage-mysql/
integration-mongo:
	go test -tags integration ./cmd/storage-mongo/

vendor: ## re-resolve and vendor dependencies into ./vendor
	GOFLAGS=-mod=mod go mod tidy
	GOFLAGS=-mod=mod go mod vendor

clean:
	rm -rf bin
