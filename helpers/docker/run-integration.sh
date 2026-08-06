#!/usr/bin/env sh
# Bring up the backing services, run every data-helper integration test against
# them, then tear down. Exits non-zero if any test fails. Designed to run in a
# clean-room with Docker available; builds from the vendored deps (offline).
#
#   helpers/docker/run-integration.sh
#
# Env knobs:
#   KEEP_UP=1   leave the compose services running after the run (for debugging)
#   COMPOSE     override the docker compose command (default: "docker compose")
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)      # helpers/docker
ROOT=$(cd "$HERE/.." && pwd)             # helpers
COMPOSE=${COMPOSE:-docker compose}
CF="$HERE/docker-compose.yml"

# Offline, reproducible build from vendor/.
export GOFLAGS=-mod=vendor
export GOTOOLCHAIN=local
export GOMODCACHE="$ROOT/.gocache"

cleanup() {
  if [ "${KEEP_UP:-0}" != "1" ]; then
    echo "--- tearing down"
    $COMPOSE -f "$CF" down >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

echo "--- bringing up services"
$COMPOSE -f "$CF" up -d

# Wait helpers ------------------------------------------------------------
wait_health() { # <service>
  echo "waiting: $1 (health)"
  i=0
  while [ "$i" -lt 60 ]; do
    cid=$($COMPOSE -f "$CF" ps -q "$1" 2>/dev/null || true)
    st=$(docker inspect --format '{{.State.Health.Status}}' "$cid" 2>/dev/null || echo none)
    [ "$st" = healthy ] && return 0
    i=$((i + 1)); sleep 2
  done
  echo "!! $1 did not become healthy"; return 1
}
wait_port() { # <host> <port> <label>
  echo "waiting: $3 ($1:$2)"
  i=0
  while [ "$i" -lt 60 ]; do
    if nc -z "$1" "$2" >/dev/null 2>&1; then return 0; fi
    i=$((i + 1)); sleep 2
  done
  echo "!! $3 not reachable on $1:$2"; return 1
}
wait_http() { # <url> <label>
  echo "waiting: $2 ($1)"
  i=0
  while [ "$i" -lt 60 ]; do
    code=$(curl -s -o /dev/null -w "%{http_code}" "$1" 2>/dev/null || echo 000)
    [ "$code" = 200 ] && return 0
    i=$((i + 1)); sleep 2
  done
  echo "!! $2 not healthy at $1"; return 1
}

wait_health postgres
wait_health mysql
wait_health mongo
wait_health rabbitmq
wait_port localhost 6379 redis
wait_port localhost 4222 nats
wait_port localhost 1883 mosquitto
wait_port localhost 9092 kafka
wait_http http://localhost:4566/_localstack/health localstack
sleep 3   # let kafka finish electing / localstack init sns,sqs

# Env for the tests -------------------------------------------------------
export CODICIS_PG_DSN='postgres://codicis:codicis@localhost:5432/codicis?sslmode=disable'
export CODICIS_MYSQL_DSN='codicis:codicis@tcp(localhost:3306)/codicis'
export CODICIS_MONGO_URI='mongodb://localhost:27017'
export CODICIS_REDIS_ADDR='localhost:6379'
export CODICIS_NATS_URL='nats://localhost:4222'
export CODICIS_KAFKA_BROKERS='localhost:9092'
export CODICIS_AMQP_URL='amqp://codicis:codicis@localhost:5672/'
export CODICIS_MQTT_URL='tcp://localhost:1883'
export AWS_ENDPOINT_URL='http://localhost:4566'
export AWS_REGION='us-east-1'
export AWS_ACCESS_KEY_ID='test'
export AWS_SECRET_ACCESS_KEY='test'

# Run every integration test ---------------------------------------------
cd "$ROOT"
fails=""
run() { # <label> <go test args...>
  label=$1
  shift
  printf '=== %s\n' "$label"
  if go test "$@"; then :; else fails="$fails $label"; fi
}

run storage-postgres -tags integration ./cmd/storage-postgres/
run storage-mysql    -tags integration ./cmd/storage-mysql/
run storage-mongo    -tags integration ./cmd/storage-mongo/
run feed-redis       -tags integration ./cmd/feed-redis/
run feed-nats        -tags integration ./cmd/feed-nats/
run feed-kafka       -tags integration -timeout 90s ./cmd/feed-kafka/
run feed-rabbitmq    -tags integration ./cmd/feed-rabbitmq/
run feed-mqtt        -tags integration ./cmd/feed-mqtt/
run feed-zeromq      ./cmd/feed-zeromq/            # pure-Go, no broker
run feed-sns         -tags integration -timeout 90s ./cmd/feed-sns/
run feed-sqs         -tags integration -timeout 90s ./cmd/feed-sqs/

if [ -n "$fails" ]; then
  echo "--- FAILED:$fails"
  exit 1
fi
echo "--- all integration tests passed"
