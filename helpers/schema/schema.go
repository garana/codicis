// Package schema embeds the reference DDL for the SQL storage helpers so a
// helper can create its tables on startup (all statements are idempotent).
package schema

import _ "embed"

//go:embed postgres.sql
var Postgres string

//go:embed mysql.sql
var MySQL string
