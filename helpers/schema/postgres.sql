-- codicis PostgreSQL storage-helper schema.
--
-- The storage helper is the system of record for the continuous resting book,
-- per-account positions, and the order/fill/trade history. Order ids are unique
-- within a symbol, so (symbol, id) is the natural key.

CREATE TABLE IF NOT EXISTS orders (
    symbol     TEXT   NOT NULL,
    id         BIGINT NOT NULL,
    owner      TEXT   NOT NULL DEFAULT '',   -- owning user UUID ('' = anon)
    side       TEXT   NOT NULL,              -- 'buy' | 'sell'
    price      BIGINT NOT NULL,              -- integer ticks
    qty        BIGINT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (symbol, id)
);

-- The resident/deep continuous book: one row per resting order.
CREATE TABLE IF NOT EXISTS resting (
    symbol TEXT   NOT NULL,
    id     BIGINT NOT NULL,
    side   TEXT   NOT NULL,
    price  BIGINT NOT NULL,
    leaves BIGINT NOT NULL,
    seq    BIGINT NOT NULL,   -- arrival sequence (time priority)
    PRIMARY KEY (symbol, id)
);
-- Serves pull_levels: best price first, seq order within a level.
CREATE INDEX IF NOT EXISTS resting_book ON resting (symbol, side, price, seq);

-- Per-account net position (buy +, sell -).
CREATE TABLE IF NOT EXISTS positions (
    owner  TEXT   NOT NULL,
    symbol TEXT   NOT NULL,
    net    BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (owner, symbol)
);

-- Append-only fill log (audit / analytics).
CREATE TABLE IF NOT EXISTS fills (
    seq_id    BIGSERIAL PRIMARY KEY,
    symbol    TEXT    NOT NULL,
    id        BIGINT  NOT NULL,
    qty       BIGINT  NOT NULL,
    remaining BIGINT  NOT NULL,
    complete  BOOLEAN NOT NULL,
    ts        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Append-only anonymous trade prints.
CREATE TABLE IF NOT EXISTS trades (
    seq_id BIGSERIAL PRIMARY KEY,
    symbol TEXT   NOT NULL,
    taker  BIGINT NOT NULL,
    maker  BIGINT NOT NULL,
    price  BIGINT NOT NULL,
    qty    BIGINT NOT NULL,
    ts     TIMESTAMPTZ NOT NULL DEFAULT now()
);
