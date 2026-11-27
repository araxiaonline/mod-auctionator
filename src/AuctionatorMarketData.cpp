
#include "AuctionatorMarketData.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    // Rows per INSERT statement. Keeps individual statements small enough for
    // max_allowed_packet while still batching the round trips.
    uint32 const ImportBatchRows = 500;

    std::string const MarketTableName = "mod_auctionator_market_price";

    // `source` value of a row this module produced by sampling the realm's own auction house,
    // so those rows can be told apart from an imported CSV feed (and pruned separately).
    std::string const ScanSource = "ah-scan";

    // Auction house prices are stored for the whole stack in columns that are unsigned, so a
    // single unit price can exceed the signed INT the market table uses. Clamping in SQL keeps
    // one absurd listing from making the whole statement fail with "Out of range value".
    // Same limit as the CSV importer (MaxImportValue below).
    uint32 const MaxScanValue = 2147483647u;

    //
    // Identity of the last imported file. The periodic import runs every
    // Auctionator.MarketData.ImportIntervalMinutes, and re-reading a file that did
    // not change would only burn world thread time.
    //
    bool lastImportValid = false;
    std::string lastImportPath;
    std::uintmax_t lastImportSize = 0;
    std::filesystem::file_time_type lastImportWriteTime{};

    std::string Trim(std::string const& value)
    {
        size_t const first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return std::string();
        }

        size_t const last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    bool TryParseUInt(std::string const& value, uint32& result)
    {
        std::string const trimmed = Trim(value);
        if (trimmed.empty() || !std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        {
            return false;
        }

        try
        {
            size_t idx = 0;
            unsigned long long const parsed = std::stoull(trimmed, &idx, 10);
            if (idx != trimmed.size() || parsed > 0xFFFFFFFFull)
            {
                return false;
            }

            result = static_cast<uint32>(parsed);
            return true;
        }
        catch (std::exception const&)
        {
            return false;
        }
    }

    // The columns this class writes (entry, average_price, buyout, bid, count) are signed
    // INT in MySQL, so a value above INT_MAX would be rejected by the server - and because
    // the rows are batched, one such row would take the whole 500 row statement (and its
    // transaction) down with it. Anything outside the column range is treated as invalid.
    uint32 const MaxImportValue = 2147483647u;

    bool TryParseColumnUInt(std::string const& value, uint32& result)
    {
        uint32 parsed = 0;
        if (!TryParseUInt(value, parsed) || parsed > MaxImportValue)
        {
            return false;
        }

        result = parsed;
        return true;
    }

    // MySQL accepts 'YYYY-MM-DD' and 'YYYY-MM-DD HH:MM:SS'. The shape is validated here
    // because the value is embedded in the generated SQL; the individual fields are range
    // checked as well, so a typo such as '2026-13-45 99:99:99' is rejected as invalid
    // instead of failing the batch that contains it.
    bool TryParseUIntField(std::string const& value, size_t offset, size_t length, uint32 minValue, uint32 maxValue)
    {
        uint32 parsed = 0;
        if (!TryParseUInt(value.substr(offset, length), parsed))
        {
            return false;
        }

        return parsed >= minValue && parsed <= maxValue;
    }

    bool IsDateTimeText(std::string const& value)
    {
        bool const withTime = value.size() == 19;
        if (value.size() != 10 && !withTime)
        {
            return false;
        }

        for (size_t i = 0; i < value.size(); ++i)
        {
            char const ch = value[i];
            bool const digitPosition = (i <= 3) || (i >= 5 && i <= 6) || (i >= 8 && i <= 9) || (i >= 11 && i <= 12) || (i >= 14 && i <= 15) || (i >= 17 && i <= 18);
            char const expected = (i == 4 || i == 7) ? '-' : (i == 10 ? ' ' : (i == 13 || i == 16) ? ':' : '\0');

            if (digitPosition)
            {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                {
                    return false;
                }
            }
            else if (ch != expected)
            {
                return false;
            }
        }

        // Ranges: year 1970..2100 keeps typos out of the history table, and day 1..31
        // accepts every month (MySQL itself rejects values such as 2026-02-30, which is a
        // one row problem at worst because the shape checks above already passed).
        return TryParseUIntField(value, 0, 4, 1970, 2100)
            && TryParseUIntField(value, 5, 2, 1, 12)
            && TryParseUIntField(value, 8, 2, 1, 31)
            && (!withTime
                || (TryParseUIntField(value, 11, 2, 0, 23)
                    && TryParseUIntField(value, 14, 2, 0, 59)
                    && TryParseUIntField(value, 17, 2, 0, 59)));
    }

    // The source ends up in the SQL as a quoted literal, so keep it boring.
    std::string SanitizeSource(std::string const& value)
    {
        std::string source;
        for (char ch : value)
        {
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.')
            {
                source.push_back(ch);
            }

            if (source.size() >= 32)
            {
                break;
            }
        }

        return source.empty() ? std::string("file") : source;
    }

    // Splits one CSV line, honouring double quoted fields ("" is an escaped quote).
    std::vector<std::string> SplitCsvLine(std::string const& line)
    {
        std::vector<std::string> fields;
        std::string current;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i)
        {
            char const ch = line[i];

            if (inQuotes)
            {
                if (ch == '"')
                {
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        current.push_back('"');
                        i++;
                    }
                    else
                    {
                        inQuotes = false;
                    }
                }
                else
                {
                    current.push_back(ch);
                }
            }
            else if (ch == '"')
            {
                inQuotes = true;
            }
            else if (ch == ',')
            {
                fields.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(ch);
            }
        }

        fields.push_back(current);
        return fields;
    }
}

AuctionatorMarketData::AuctionatorMarketData()
{
    SetLogPrefix("[AuctionatorMarket] ");
}

bool AuctionatorMarketData::TableIsReady()
{
    QueryResult result = CharacterDatabase.Query(R"(
        SELECT COUNT(*)
        FROM INFORMATION_SCHEMA.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = '{}'
          AND COLUMN_NAME IN ('source', 'imported_at')
    )", MarketTableName);

    if (!result)
    {
        logError("cannot inspect " + MarketTableName + " (is the characters database reachable?)");
        return false;
    }

    uint64 const columnCount = result->Fetch()[0].Get<uint64>();
    if (columnCount < 2)
    {
        logError(MarketTableName + " is missing the source/imported_at columns: apply the module SQL updates "
            "(data/sql/db-characters/updates/2026_09_20_00_market_price_history.sql) or check Updates.EnableDatabases.");
        return false;
    }

    return true;
}

AuctionatorMarketData::ImportResult AuctionatorMarketData::ImportFromFile(std::string const& path,
    std::string const& source, uint32 maxRows, bool force)
{
    ImportResult result;
    result.path = path;

    if (path.empty())
    {
        logWarn("market import skipped: no Auctionator.MarketData.ImportFile configured.");
        return result;
    }

    if (!TableIsReady())
    {
        return result;
    }

    std::error_code errorCode;
    std::filesystem::path const filePath(path);

    if (!std::filesystem::exists(filePath, errorCode) || errorCode)
    {
        logWarn("market import skipped: file not found: " + std::filesystem::absolute(filePath, errorCode).string());
        return result;
    }

    // Both calls signal failure through errorCode. Without checking it, the "unchanged file"
    // comparison further down would run on indeterminate size/mtime values and could then skip
    // a file that did change (or re-import one that did not).
    std::uintmax_t const fileSize = std::filesystem::file_size(filePath, errorCode);
    if (errorCode)
    {
        logError("market import failed: cannot stat " + path + " (" + errorCode.message() + ")");
        return result;
    }

    std::filesystem::file_time_type const writeTime = std::filesystem::last_write_time(filePath, errorCode);
    if (errorCode)
    {
        logError("market import failed: cannot read the modification time of " + path + " (" + errorCode.message() + ")");
        return result;
    }

    if (!force && lastImportValid && lastImportPath == path
        && lastImportSize == fileSize && lastImportWriteTime == writeTime)
    {
        result.ok = true;
        result.unchanged = true;
        logDebug("market import skipped: " + path + " is unchanged since the last import.");
        return result;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        logError("market import failed: cannot open " + path);
        return result;
    }

    std::string const sanitizedSource = SanitizeSource(source);

    auto trans = CharacterDatabase.BeginTransaction();
    std::string batch;
    uint32 batchRows = 0;

    auto flushBatch = [&]()
    {
        if (batchRows == 0)
        {
            return;
        }

        // No backticks around `count` in the VALUES tuple, but they are required in
        // the assignments because COUNT is a keyword there.
        trans->Append("INSERT INTO " + MarketTableName
            + " (entry, average_price, buyout, bid, `count`, scan_datetime, source) VALUES "
            + batch
            + " ON DUPLICATE KEY UPDATE average_price=VALUES(average_price), buyout=VALUES(buyout),"
              " bid=VALUES(bid), `count`=VALUES(`count`), source=VALUES(source), imported_at=CURRENT_TIMESTAMP");

        result.batches++;
        batch.clear();
        batchRows = 0;
    };

    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line.empty())
        {
            continue;
        }

        result.read++;

        std::vector<std::string> const fields = SplitCsvLine(line);

        // scan_datetime, item_entry, avg_price, ...
        if (fields.size() < 3)
        {
            result.invalid++;
            continue;
        }

        std::string const scanDatetime = Trim(fields[0]);
        uint32 entry = 0;
        uint32 averagePrice = 0;

        // A header line fails here and is counted as invalid, which is intended:
        // the import does not require a header but tolerates one.
        if (!IsDateTimeText(scanDatetime)
            || !TryParseColumnUInt(fields[1], entry)
            || !TryParseColumnUInt(fields[2], averagePrice)
            || entry == 0
            || averagePrice == 0)
        {
            result.invalid++;
            continue;
        }

        if (maxRows > 0 && result.imported >= maxRows)
        {
            result.truncated = true;
            break;
        }

        // Optional columns: an unparsable or out of range value only loses that column
        // (these are informational: `count` feeds the seller's volume weighting).
        uint32 buyout = 0;
        uint32 bid = 0;
        uint32 count = 0;
        if (fields.size() > 3) { TryParseColumnUInt(fields[3], buyout); }
        if (fields.size() > 4) { TryParseColumnUInt(fields[4], bid); }
        if (fields.size() > 5) { TryParseColumnUInt(fields[5], count); }

        if (batchRows > 0)
        {
            batch += ",";
        }

        batch += "(" + std::to_string(entry)
            + "," + std::to_string(averagePrice)
            + "," + std::to_string(buyout)
            + "," + std::to_string(bid)
            + "," + std::to_string(count)
            + ",'" + scanDatetime + "','" + sanitizedSource + "')";

        batchRows++;
        result.imported++;

        if (batchRows >= ImportBatchRows)
        {
            flushBatch();
        }
    }

    flushBatch();
    CharacterDatabase.CommitTransaction(trans);

    // The file is remembered even when every line was rejected: re-reading it on every cycle
    // would not change the outcome, and "marketimport force" is available when it does change.
    lastImportValid = true;
    lastImportPath = path;
    lastImportSize = fileSize;
    lastImportWriteTime = writeTime;

    //
    // A file that produced nothing usable (every data line unparsable, out of range or
    // without an item id/price) is a failure, not a success: reporting ok would hide a
    // broken export behind a table that silently stays empty. An empty file stays a
    // successful no-op.
    //
    if (result.imported == 0 && result.read > 0)
    {
        logError("market import from " + path + " wrote 0 rows: all " + std::to_string(result.read)
            + " line(s) were rejected (expected scan_datetime,item_entry,avg_price,minimum_buyout,"
              "minimum_bid,item_count with a 'YYYY-MM-DD[ HH:MM:SS]' date, a non-zero item entry "
              "and a non-zero average price).");
        return result;
    }

    result.ok = true;

    logInfo("market import from " + path + " (source '" + sanitizedSource + "'): "
        + std::to_string(result.imported) + " row(s) written in " + std::to_string(result.batches) + " statement(s), "
        + std::to_string(result.invalid) + " line(s) skipped, " + std::to_string(result.read) + " line(s) read"
        + (result.truncated ? " - TRUNCATED at Auctionator.MarketData.ImportMaxRows" : ""));

    return result;
}

uint32 AuctionatorMarketData::PruneOlderThan(uint32 days)
{
    if (days < 1)
    {
        logError("market prune refused: the retention must be at least 1 day.");
        return 0;
    }

    if (!TableIsReady())
    {
        return 0;
    }

    QueryResult countResult = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM " + MarketTableName + " WHERE scan_datetime < DATE_SUB(NOW(), INTERVAL {} DAY)", days);

    uint64 const rowCount = countResult ? countResult->Fetch()[0].Get<uint64>() : 0;
    if (rowCount == 0)
    {
        logInfo("market prune: nothing older than " + std::to_string(days) + " day(s).");
        return 0;
    }

    // The count above is exact for the delete below: both run on the world thread, which is
    // also the only writer of this table (the timer import and the GM command), so nothing
    // can be inserted between them.
    CharacterDatabase.Execute(
        "DELETE FROM " + MarketTableName + " WHERE scan_datetime < DATE_SUB(NOW(), INTERVAL {} DAY)", days);

    logInfo("market prune: removed " + std::to_string(rowCount) + " row(s) older than " + std::to_string(days) + " day(s).");
    return static_cast<uint32>(std::min<uint64>(rowCount, 0xFFFFFFFFull));
}

AuctionatorMarketData::ScanResult AuctionatorMarketData::ScanAuctionHouse(uint32 excludeOwner)
{
    ScanResult result;

    if (!TableIsReady())
    {
        logWarn("market scan skipped: " + MarketTableName + " is not ready.");
        return result;
    }

    // One timestamp for the whole batch, and the database produces it: the readers age a scan
    // with TIMESTAMPDIFF against NOW(), so both sides have to use the database's own clock and
    // time zone. It is also what makes the rows of this scan countable afterwards.
    uint64 const scanEpoch = static_cast<uint64>(std::time(nullptr));
    std::string const scanDatetime = "FROM_UNIXTIME(" + std::to_string(scanEpoch) + ")";

    // How much there is to sample and how much of it belongs to the excluded owner. Reported by
    // the caller, so a scan that writes nothing can say why instead of just looking broken
    // (which is exactly what happens on a realm whose house only holds the bot's own listings).
    std::string const compositionSql =
        "SELECT COUNT(*), COALESCE(SUM(ah.itemowner = " + std::to_string(excludeOwner) + "), 0)"
        " FROM auctionhouse ah"
        " INNER JOIN item_instance ii ON ii.guid = ah.itemguid"
        " WHERE ii.itemEntry > 0 AND ii.count > 0";

    if (QueryResult composition = CharacterDatabase.Query(compositionSql))
    {
        Field* fields = composition->Fetch();
        result.totalListings = fields[0].Get<uint32>();
        result.skippedSelf = fields[1].Get<uint32>();
    }

    if (result.totalListings == 0)
    {
        // An empty house is not an error: there is simply nothing to price yet.
        logInfo("market scan: the auction house holds no usable listing.");
        result.ok = true;
        return result;
    }

    // The excluded owner is only ever an integer from the configuration, so it is safe to inline
    // (and it keeps this statement free of format placeholders, which the SQL itself would
    // otherwise have to escape).
    std::string const ownerFilter = excludeOwner != 0
        ? " AND ah.itemowner <> " + std::to_string(excludeOwner)
        : "";

    //
    // The whole aggregation is this one statement; the module only supplies the owner filter and
    // the scan timestamp.
    //
    // Prices in `auctionhouse` are stored for the entire stack, while the market table holds a
    // per-unit price (the seller multiplies it by the stack itself), so every price is divided by
    // item_instance.count.
    //
    // average_price is the traded-volume weighted unit price: a buyout where the listing has one,
    // and only for items whose listings carry no buyout at all the start bid, so a realm running
    // bid-only listings still gets a price instead of nothing while items that do have buyouts are
    // not dragged down by reserves.
    //
    // `buyout`/`bid` keep the cheapest unit buyout/start bid seen, matching the CSV export's
    // minimum_buyout/minimum_bid columns (the seller and the bidder only read average_price and
    // the volume, but the columns belong to the table's format).
    //
    std::string const scanSql =
        "INSERT INTO " + MarketTableName
        + " (entry, average_price, buyout, bid, `count`, scan_datetime, source)"
          " SELECT s.entry, LEAST(" + std::to_string(MaxScanValue) + ", GREATEST(1, s.average_price)),"
          " LEAST(" + std::to_string(MaxScanValue) + ", s.min_buyout),"
          " LEAST(" + std::to_string(MaxScanValue) + ", s.min_bid),"
          " s.listings, " + scanDatetime + ", '" + ScanSource + "'"
          " FROM ("
          "  SELECT ii.itemEntry AS entry,"
          "   COALESCE("
          "    ROUND(SUM(CASE WHEN ah.buyoutprice > 0 THEN ah.buyoutprice END)"
          "          / NULLIF(SUM(CASE WHEN ah.buyoutprice > 0 THEN ii.count END), 0)),"
          "    ROUND(SUM(CASE WHEN ah.startbid > 0 THEN ah.startbid END)"
          "          / NULLIF(SUM(CASE WHEN ah.startbid > 0 THEN ii.count END), 0)),"
          "    0) AS average_price,"
          "   COALESCE("
          "    MIN(CASE WHEN ah.buyoutprice > 0 THEN ROUND(ah.buyoutprice / ii.count) END),"
          "    MIN(CASE WHEN ah.startbid > 0 THEN ROUND(ah.startbid / ii.count) END),"
          "    0) AS min_buyout,"
          "   COALESCE(MIN(CASE WHEN ah.startbid > 0 THEN ROUND(ah.startbid / ii.count) END), 0) AS min_bid,"
          "   COUNT(*) AS listings"
          "  FROM auctionhouse ah"
          "  INNER JOIN item_instance ii ON ii.guid = ah.itemguid"
          "  WHERE ii.itemEntry > 0 AND ii.count > 0" + ownerFilter
        + "  GROUP BY ii.itemEntry"
          " ) s"
          " WHERE s.average_price >= 1";

    // DirectExecute, not Execute: the count below has to see this statement's rows, and Execute
    // only queues the query on the database thread pool. (Neither returns a status - a rejected
    // statement lands in the sql log, which is why the empty-result case below names it.)
    CharacterDatabase.DirectExecute(scanSql);

    // Exact for what was just written: the world thread is the only writer of this table (the
    // timer import, the GM commands and this scan), and the timestamp identifies the batch.
    std::string const countSql =
        "SELECT COUNT(*), COALESCE(CAST(SUM(`count`) AS UNSIGNED), 0) FROM " + MarketTableName
        + " WHERE scan_datetime = " + scanDatetime + " AND source = '" + ScanSource + "'";

    if (QueryResult written = CharacterDatabase.Query(countSql))
    {
        Field* fields = written->Fetch();
        result.rows = fields[0].Get<uint32>();
        result.listings = fields[1].Get<uint32>();
    }

    result.ok = true;

    if (result.rows == 0)
    {
        if (excludeOwner != 0 && result.skippedSelf == result.totalListings)
        {
            // The common case on a realm the bot has to itself: everything in the house belongs
            // to the auctionator, so excluding it leaves nothing to price. Say so explicitly -
            // the timer path has no GM reading a chat reply, and "wrote nothing" on its own
            // looks like a broken scan.
            logWarn("market scan wrote nothing: all " + std::to_string(result.totalListings)
                + " usable listing(s) in the house belong to the auctionator itself, and "
                  "Auctionator.MarketData.ScanExcludeSelf leaves those out. Set it to 0 to sample them too, "
                  "or point Auctionator.MarketData.ImportFile at an external export.");
        }
        else
        {
            logWarn("market scan wrote nothing although the house holds " + std::to_string(result.totalListings)
                + " usable listing(s) and " + std::to_string(result.totalListings - result.skippedSelf)
                + " were sampled. If the aggregation was rejected the reason is in the sql log; "
                  "otherwise no sampled listing carried a price at all.");
        }
    }
    else
    {
        logInfo("market scan: wrote " + std::to_string(result.rows) + " item price(s) from "
            + std::to_string(result.listings) + " of " + std::to_string(result.totalListings)
            + " listing(s)"
            + (excludeOwner != 0
                ? " (excluded " + std::to_string(result.skippedSelf) + " belonging to the auctionator)."
                : " (the auctionator's own listings included)."));
    }

    return result;
}

bool AuctionatorMarketData::GetStats(uint32 maxAgeDays, Stats& stats)
{
    if (!TableIsReady())
    {
        return false;
    }

    // maxAgeDays = 0 means "no age limit", so every item counts as fresh.
    QueryResult result = CharacterDatabase.Query(R"(
        SELECT
            COUNT(*)
            , COUNT(DISTINCT entry)
            , COUNT(DISTINCT CASE WHEN {} = 0 OR scan_datetime >= DATE_SUB(NOW(), INTERVAL {} DAY) THEN entry END)
            , COALESCE(DATE_FORMAT(MAX(scan_datetime), '%Y-%m-%d %H:%i:%s'), 'n/a')
            , COALESCE(DATE_FORMAT(MIN(scan_datetime), '%Y-%m-%d %H:%i:%s'), 'n/a')
        FROM mod_auctionator_market_price
    )", maxAgeDays, maxAgeDays);

    if (!result)
    {
        logError("market stats query failed.");
        return false;
    }

    Field* fields = result->Fetch();
    stats.totalRows = fields[0].Get<uint64>();
    stats.distinctItems = fields[1].Get<uint32>();
    stats.freshItems = fields[2].Get<uint32>();
    stats.newestScan = fields[3].Get<std::string>();
    stats.oldestScan = fields[4].Get<std::string>();

    return true;
}
