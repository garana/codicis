# codicis data helpers (Go)

Reference **storage** and **feed** helpers for codicis, written in Go. codicis
delegates persistence and market-data fan-out to child-process helpers over
pipes; a helper is any program that speaks the documented wire protocol, so
these are **reference implementations** -- an operator may replace any of them
with their own (in any language) as long as the protocol matches.

Go is used here because the mature, well-supported client libraries for these
databases and brokers live in the Go/Java/etc. ecosystems, and the
helper-process boundary keeps those third-party dependencies **out of the
matching engine's address space**: each helper links only its own driver, so a
bug or CVE in a driver can at worst crash a restartable child, never the matcher.

## Layout

```
internal/wire       the codicis helper text codec ("key=value" line protocol)
internal/storage    shared storage-helper runtime (Store interface + dispatch)
internal/feed       shared feed decode + subscription (added with the feed helpers)
schema/             reference DDL (postgres.sql, mysql.sql) + embed
cmd/storage-*       one storage-helper binary per database
cmd/feed-*          one feed-fanout binary per broker (added incrementally)
docker/             docker-compose backing services for integration tests
```

The shared runtime handles the protocol once; each backend supplies only a thin
`Store` (or feed `Sink`) adapter. Because Go statically links only imported
packages, each binary depends on the fewest libraries -- e.g. `storage-postgres`
pulls in `pgx` and nothing MySQL/Mongo-related.

## Build

Dependencies are **vendored** under `vendor/`, and when re-resolving they are
cached in a repo-local `.gocache` (never the global module cache), so builds are
reproducible and self-contained:

```
cd helpers
make build          # -> bin/storage-postgres, bin/storage-mysql, bin/storage-mongo
make test           # unit tests (wire codec + protocol dispatch; no services)
make vendor         # re-resolve + re-vendor after changing deps
```

## Storage helpers

| Backend    | Binary             | Driver                          | Config                    |
| ---------- | ------------------ | ------------------------------- | ------------------------- |
| PostgreSQL | `storage-postgres` | jackc/pgx                       | `-dsn` / `$CODICIS_PG_DSN`   |
| MySQL      | `storage-mysql`    | go-sql-driver/mysql             | `-dsn` / `$CODICIS_MYSQL_DSN`|
| MongoDB    | `storage-mongo`    | go.mongodb.org/mongo-driver     | `-uri` / `$CODICIS_MONGO_URI`|

Point codicis at one via `storage.helper_cmd`, e.g.:

```
storage.helper_cmd = /path/to/storage-postgres -dsn postgres://.../codicis -migrate
```

`-migrate` applies the schema on startup: for SQL that is idempotent
`CREATE TABLE IF NOT EXISTS` (`schema/postgres.sql`, `schema/mysql.sql`); for
Mongo it installs the `$jsonSchema` validators (`schema/mongo.js`) that enforce
field presence and BSON types (rejecting e.g. a string price -- the class of bug
a schemaless store would otherwise accept). Writes (`report_*`, `commit`) are
applied in strict arrival order;
reads (`pull_levels`, `pull_position`) run concurrently on the connection pool
(`-read-concurrency`, default 8) -- matching the reader/writer split codicis
expects.

### Wire protocol (so you can write your own)

Text codec: one record is `req_id=<n>`, `type=<t>`, then `key=value` lines, then
a blank line. `req_id`/`type` are reserved. Requests are pipelined and a helper
may answer out of order (the response echoes `req_id`).

| Request         | Fields                                   | Response                       |
| --------------- | ---------------------------------------- | ------------------------------ |
| `report_order`  | symbol, owner, id, side, price, qty      | status=ok                      |
| `report_rest`   | symbol, side, id, price, leaves, seq     | status=ok                      |
| `report_fill`   | symbol, id, qty, remaining, status       | status=ok                      |
| `report_cancel` | symbol, id                               | status=ok                      |
| `report_trade`  | symbol, taker, maker, price, qty         | status=ok                      |
| `commit`        | (none)                                   | committed=<highest write id>   |
| `pull_position` | user, symbol                             | net=<int>                      |
| `pull_levels`   | symbol, side, from_price, count          | symbol, side, orders=<blob>, count |
| `ping`          | (none)                                   | (type pong)                    |

`report_fill` decrements the resting order (removing it when `status=filled`)
and updates the owner's net position (`buy` +, `sell` -). `pull_levels` returns
the orders on `side` beyond `from_price` (worse than it) across at most `count`
price levels, best price first and arrival (`seq`) order within a level; the
`orders` blob is `id,price,leaves,seq` records joined by `;`.

## Integration tests (Docker)

```
docker compose -f docker/docker-compose.yml up -d      # postgres, mysql, mongo
cd helpers
CODICIS_PG_DSN='postgres://codicis:codicis@localhost:5432/codicis?sslmode=disable' \
  go test -tags integration ./cmd/storage-postgres/
CODICIS_MYSQL_DSN='codicis:codicis@tcp(localhost:3306)/codicis' \
  go test -tags integration ./cmd/storage-mysql/
CODICIS_MONGO_URI='mongodb://localhost:27017' \
  go test -tags integration ./cmd/storage-mongo/
```

Each test drives its `Store` through a full order lifecycle (rest -> pull ->
partial fill -> position -> complete fill -> cancel) against the real database.
Without the env var set, the test skips.

### One command (CI / clean-room)

`make integration` runs `docker/run-integration.sh`, which brings up every
backing service, waits for readiness, runs all storage + feed integration tests
against them with the right env, tears down, and exits non-zero on any failure.
It builds from `vendor/` (offline) and is what a clean-room / CI job invokes:

```
make -C helpers integration        # ~30s once images are pulled
KEEP_UP=1 make -C helpers integration   # leave services up for debugging
```

## Feed helpers

The market-data fan-out bridges subscribe to the codicis feed-helper's TCP
stream (newline JSON: an L1 snapshot on connect, then L1 updates and any
l2/l2d/l3 query replies) and republish each per-symbol message to a broker. The
shared `internal/feed` runtime does the connect / read / reconnect loop once
(the feed is best-effort: on a dropped connection the bridge reconnects and the
helper re-snapshots); each bridge binary supplies only a `Sink`.

| Broker        | Binary          | Client (pure-Go)        | Destination                       |
| ------------- | --------------- | ----------------------- | --------------------------------- |
| Redis         | `feed-redis`    | redis/go-redis          | PUBLISH channel `<prefix><sym>`   |
| NATS          | `feed-nats`     | nats-io/nats.go         | subject `<prefix><sym>`           |
| Kafka         | `feed-kafka`    | segmentio/kafka-go      | topic `<prefix><sym>`, keyed by sym |
| RabbitMQ/AMQP | `feed-rabbitmq` | rabbitmq/amqp091-go     | topic exchange, routing `<prefix><sym>` |
| MQTT          | `feed-mqtt`     | eclipse/paho.mqtt.golang| topic `<prefix><sym>`             |
| ZeroMQ        | `feed-zeromq`   | go-zeromq/zmq4          | PUB `[<prefix><sym>, payload]`    |
| AWS SNS       | `feed-sns`      | aws-sdk-go-v2           | Publish to a topic ARN            |
| AWS SQS       | `feed-sqs`      | aws-sdk-go-v2           | SendMessage to a queue URL        |

Every client is pure-Go (no CGO / no system C libs), so each binary links only
its own driver. Run a bridge against a running feed-helper:

```
feed-redis    -feed 127.0.0.1:9100 -redis localhost:6379 -prefix md.
feed-kafka    -feed 127.0.0.1:9100 -brokers localhost:9092 -prefix md.
feed-rabbitmq -feed 127.0.0.1:9100 -amqp amqp://guest:guest@localhost:5672/
feed-sqs      -feed 127.0.0.1:9100 -queue "$SQS_URL" -endpoint "$AWS_ENDPOINT_URL"
```

Integration tests (gated on the broker env var) stand up a fake feed source and
assert the bridge republishes to the real broker:

```
docker compose -f docker/docker-compose.yml up -d      # all brokers
CODICIS_REDIS_ADDR=localhost:6379 go test -tags integration ./cmd/feed-redis/
CODICIS_NATS_URL=nats://localhost:4222 go test -tags integration ./cmd/feed-nats/
CODICIS_KAFKA_BROKERS=localhost:9092  go test -tags integration ./cmd/feed-kafka/
CODICIS_AMQP_URL=amqp://codicis:codicis@localhost:5672/ \
  go test -tags integration ./cmd/feed-rabbitmq/
CODICIS_MQTT_URL=tcp://localhost:1883 go test -tags integration ./cmd/feed-mqtt/
go test ./cmd/feed-zeromq/            # ZeroMQ is pure-Go: no broker needed
AWS_ENDPOINT_URL=http://localhost:4566 AWS_REGION=us-east-1 \
  AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
  go test -tags integration ./cmd/feed-sqs/ ./cmd/feed-sns/   # LocalStack
```

## Third-party licenses

Every vendored dependency is permissively licensed and **none are GPL/LGPL**, so
vendoring and redistribution are compatible with codicis's own license:

- MIT: pgx, segmentio/kafka-go, xxhash, montanaflynn/stats, youmark/pkcs8
- Apache-2.0: mongo-driver, nats.go, aws-sdk-go-v2, xdg-go/*
- BSD-2/3: redis/go-redis, rabbitmq/amqp091-go, go-zeromq/zmq4, golang.org/x/*,
  gorilla/websocket, pierrec/lz4, snappy, klauspost/compress
- MPL-2.0 (weak, file-level; used unmodified): go-sql-driver/mysql
- EPL-2.0 (weak, file-level; used unmodified): eclipse/paho.mqtt.golang -- only
  the `feed-mqtt` binary; an operator wanting strictly-permissive-only deps can
  skip it.

Each dependency's license text is retained under `vendor/`.
