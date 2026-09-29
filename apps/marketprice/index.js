//
// mod-auctionator market price importer.
//
// Reads a CSV export and writes SQL to stdout, so it can be piped straight into
// the characters database:
//
//     node index.js mypricedata.csv --source=tsm | mysql -u <dbuser> -p <characters_db>
//
// The generated SQL is idempotent: rows are inserted with
// "ON DUPLICATE KEY UPDATE" against the (entry, scan_datetime) primary key, so
// re-importing the same export updates instead of failing, and importing an
// export that only covers part of the catalogue never touches the other items.
//
// Expected columns (an optional first line is treated as a header and skipped):
//     scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count
//
// Progress goes to stderr, the SQL to stdout, so a broken pipe is not polluted.
//
// Options:
//     --source=<name>   value stored in the `source` column (default "csv")
//     --table=<name>    destination table (default mod_auctionator_market_price)
//     --batch=<n>       rows per INSERT statement (default 500)
//     --help
//

const fs = require("fs");
// csv-parse is the parser itself; it is declared in package.json now (it used to work
// only because it happened to be hoisted as a transitive dependency of `csv`).
const { parse } = require("csv-parse");

const DEFAULTS = {
  source: "csv",
  table: "mod_auctionator_market_price",
  batch: 500,
};

function parseArgs(argv) {
  const options = { ...DEFAULTS };
  const positional = [];

  for (const arg of argv) {
    if (arg === "--help" || arg === "-h") {
      options.help = true;
    } else if (arg.startsWith("--source=")) {
      options.source = arg.slice("--source=".length);
    } else if (arg.startsWith("--table=")) {
      options.table = arg.slice("--table=".length);
    } else if (arg.startsWith("--batch=")) {
      options.batch = Number.parseInt(arg.slice("--batch=".length), 10);
    } else if (arg.startsWith("--")) {
      throw new Error(`Unknown option: ${arg}`);
    } else {
      positional.push(arg);
    }
  }

  if (positional.length !== 1) {
    throw new Error("You must specify exactly one filename for importing");
  }

  options.filename = positional[0];

  if (!Number.isInteger(options.batch) || options.batch < 1 || options.batch > 10000) {
    throw new Error("--batch must be an integer between 1 and 10000");
  }

  // Keep the identifiers we inject into SQL strictly alphanumeric.
  if (!/^[A-Za-z0-9_]+$/.test(options.table)) {
    throw new Error("--table must match [A-Za-z0-9_]+");
  }

  options.source = String(options.source).replace(/[^A-Za-z0-9_.-]/g, "").slice(0, 32);
  if (options.source.length === 0) {
    options.source = DEFAULTS.source;
  }

  return options;
}

function usage() {
  console.error(`Usage: node index.js <file.csv> [--source=name] [--table=name] [--batch=n]

CSV columns (header line is skipped):
  scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count`);
}

function isUInt(value) {
  const text = String(value ?? "").trim();
  return /^\d+$/.test(text);
}

function isDateTime(value) {
  const text = String(value ?? "").trim();
  return /^\d{4}-\d{2}-\d{2}( \d{2}:\d{2}:\d{2})?$/.test(text);
}

function sqlUInt(value) {
  // Clamp to the range of the signed INT columns of the destination table (the C++
  // importer applies the same limit): a larger value would make the whole piped
  // transaction fail with "Out of range value".
  const parsed = Number.parseInt(String(value).trim(), 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return "0";
  }
  return String(Math.min(parsed, 2147483647));
}

class SqlWriter {
  constructor(table, source, batchSize) {
    this.table = table;
    this.source = source;
    this.batchSize = batchSize;
    this.buffer = [];
    this.batches = 0;
    this.started = false;
  }

  start() {
    process.stdout.write("START TRANSACTION;\n");
    this.started = true;
  }

  add(row) {
    this.buffer.push(
      `(${sqlUInt(row.entry)},${sqlUInt(row.averagePrice)},${sqlUInt(row.buyout)},${sqlUInt(row.bid)},${sqlUInt(row.count)},'${row.scanDatetime}','${this.source}')`
    );

    if (this.buffer.length >= this.batchSize) {
      this.flush();
    }
  }

  flush() {
    if (this.buffer.length === 0) {
      return;
    }

    process.stdout.write(
      `INSERT INTO \`${this.table}\` (\`entry\`, \`average_price\`, \`buyout\`, \`bid\`, \`count\`, \`scan_datetime\`, \`source\`) VALUES\n` +
        this.buffer.join(",\n") +
        `\nON DUPLICATE KEY UPDATE \`average_price\`=VALUES(\`average_price\`), \`buyout\`=VALUES(\`buyout\`), \`bid\`=VALUES(\`bid\`), \`count\`=VALUES(\`count\`), \`source\`=VALUES(\`source\`), \`imported_at\`=CURRENT_TIMESTAMP;\n`
    );

    this.batches++;
    this.buffer = [];
  }

  commit() {
    this.flush();
    if (this.started) {
      process.stdout.write("COMMIT;\n");
    }
  }
}

async function main() {
  let options;
  try {
    options = parseArgs(process.argv.slice(2));
  } catch (error) {
    console.error(error.message);
    usage();
    process.exit(1);
  }

  if (options.help) {
    usage();
    return;
  }

  if (!fs.existsSync(options.filename)) {
    console.error(`File not found: ${options.filename}`);
    process.exit(1);
  }

  const writer = new SqlWriter(options.table, options.source, options.batch);
  const counters = { read: 0, imported: 0, skipped: 0 };

  writer.start();

  const parser = fs
    .createReadStream(options.filename)
    .pipe(parse({ delimiter: ",", relax_column_count: true }));

  // The header line is optional: skip the first row only when it is not a data row, so a
  // headerless export does not lose its first item.
  let firstRow = true;

  for await (const row of parser) {
    if (firstRow) {
      firstRow = false;
      if (!isDateTime(String(row[0] ?? "").trim())) {
        continue;
      }
    }

    counters.read++;

    // scan_datetime, item_entry, avg_price, minimum_buyout, minimum_bid, item_count
    const scanDatetime = String(row[0] ?? "").trim();
    const entry = String(row[1] ?? "").trim();
    const averagePrice = String(row[2] ?? "").trim();

    if (!isDateTime(scanDatetime) || !isUInt(entry) || !isUInt(averagePrice)) {
      counters.skipped++;
      continue;
    }

    if (Number.parseInt(entry, 10) === 0 || Number.parseInt(averagePrice, 10) === 0) {
      // No item id or no price: the row carries no usable information.
      counters.skipped++;
      continue;
    }

    writer.add({
      scanDatetime,
      entry,
      averagePrice,
      buyout: isUInt(row[3]) ? row[3] : 0,
      bid: isUInt(row[4]) ? row[4] : 0,
      count: isUInt(row[5]) ? row[5] : 0,
    });

    counters.imported++;

    if (counters.imported % 5000 === 0) {
      console.error(`... ${counters.imported} rows`);
    }
  }

  writer.commit();

  console.error(
    `Imported ${counters.imported} row(s) (${counters.skipped} skipped, ${counters.read} read) ` +
      `into ${options.table} as source "${options.source}" in ${writer.batches} statement(s).`
  );
  console.error("Remember to prune history now and then: DELETE FROM mod_auctionator_market_price WHERE scan_datetime < NOW() - INTERVAL 30 DAY;");
}

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exit(1);
});
