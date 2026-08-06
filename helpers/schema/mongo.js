// codicis MongoDB storage-helper schema (reference).
//
// Mongo is schemaless, so the storage helper applies these $jsonSchema
// validators (equivalently, in Go, on `storage-mongo -migrate`) to enforce
// field presence and BSON types -- recovering the type safety the SQL helpers
// get from their DDL, which matters most precisely because Mongo would
// otherwise store a string price without complaint.
//
// Run manually with:  mongosh "<uri>/codicis" schema/mongo.js
//
// Numeric fields accept ["int","long"] (the 64-bit engine). A wide-precision
// (128/256-bit) market would live in a SEPARATE database/collections whose
// validators accept "decimal" instead -- the DB mirrors the C++ Order<> width
// per market (no single "widest" collection).

const num = ["int", "long"];
const S = { bsonType: "string" };
const N = { bsonType: num };
const side = { enum: ["buy", "sell"] };

const schemas = {
  orders: {
    required: ["_id", "symbol", "owner", "side", "price", "qty"],
    properties: { _id: S, symbol: S, owner: S, side: side, price: N, qty: N },
  },
  resting: {
    required: ["_id", "symbol", "side", "price", "leaves", "seq"],
    properties: { _id: S, symbol: S, side: side, price: N, leaves: N, seq: N },
  },
  positions: {
    required: ["_id", "owner", "symbol", "net"],
    properties: { _id: S, owner: S, symbol: S, net: N },
  },
  fills: {
    required: ["symbol", "id", "qty", "remaining", "complete"],
    properties: { symbol: S, id: N, qty: N, remaining: N, complete: { bsonType: "bool" } },
  },
  trades: {
    required: ["symbol", "taker", "maker", "price", "qty"],
    properties: { symbol: S, taker: N, maker: N, price: N, qty: N },
  },
};

for (const [name, props] of Object.entries(schemas)) {
  const validator = { $jsonSchema: { bsonType: "object", ...props } };
  const exists = db.getCollectionNames().includes(name);
  if (exists) {
    db.runCommand({ collMod: name, validator: validator,
      validationLevel: "moderate", validationAction: "error" });
  } else {
    db.createCollection(name, { validator: validator,
      validationLevel: "moderate", validationAction: "error" });
  }
}

// Index that serves pull_levels (best price first, seq order within a level).
db.resting.createIndex({ symbol: 1, side: 1, price: 1, seq: 1 });
print("codicis mongo validators + index applied");
