-- codicis MySQL storage-helper schema.
--
-- Mirrors the PostgreSQL schema; MySQL needs VARCHAR (not TEXT) for keys and
-- AUTO_INCREMENT for the log tables. Order ids are unique within a symbol.

CREATE TABLE IF NOT EXISTS orders (
    symbol     VARCHAR(32)         NOT NULL,
    id         BIGINT UNSIGNED     NOT NULL,
    owner      VARCHAR(64)         NOT NULL DEFAULT '',
    side       VARCHAR(8)          NOT NULL,
    price      BIGINT              NOT NULL,
    qty        BIGINT              NOT NULL,
    created_at TIMESTAMP           NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (symbol, id)
);

CREATE TABLE IF NOT EXISTS resting (
    symbol VARCHAR(32)     NOT NULL,
    id     BIGINT UNSIGNED NOT NULL,
    side   VARCHAR(8)      NOT NULL,
    price  BIGINT          NOT NULL,
    leaves BIGINT          NOT NULL,
    seq    BIGINT          NOT NULL,
    PRIMARY KEY (symbol, id),
    KEY resting_book (symbol, side, price, seq)
);

CREATE TABLE IF NOT EXISTS positions (
    owner  VARCHAR(64) NOT NULL,
    symbol VARCHAR(32) NOT NULL,
    net    BIGINT      NOT NULL DEFAULT 0,
    PRIMARY KEY (owner, symbol)
);

CREATE TABLE IF NOT EXISTS fills (
    seq_id    BIGINT AUTO_INCREMENT PRIMARY KEY,
    symbol    VARCHAR(32) NOT NULL,
    id        BIGINT UNSIGNED NOT NULL,
    qty       BIGINT      NOT NULL,
    remaining BIGINT      NOT NULL,
    complete  TINYINT(1)  NOT NULL,
    ts        TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS trades (
    seq_id BIGINT AUTO_INCREMENT PRIMARY KEY,
    symbol VARCHAR(32)     NOT NULL,
    taker  BIGINT UNSIGNED NOT NULL,
    maker  BIGINT UNSIGNED NOT NULL,
    price  BIGINT          NOT NULL,
    qty    BIGINT          NOT NULL,
    ts     TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP
);
