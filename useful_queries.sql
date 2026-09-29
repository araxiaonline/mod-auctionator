-- ---------------------------------------------------------------------------
-- useful_queries.sql - developer/operator helper queries. NOTHING HERE IS RUN BY
-- THE MOD, and nothing here is part of the module's SQL updates.
--
-- !!  READ BEFORE RUNNING  !!
--   * Every statement except the "operating" section at the bottom is a plain
--     SELECT and safe to run.
--   * The statements in "OPERATING (DESTRUCTIVE)" are COMMENTED OUT on purpose.
--     Uncomment them one at a time and read what they do first: they delete
--     auctions, items and mail. The README documents the reset as well.
--   * The module's own queries never hardcode a schema name: AuctionatorSeller
--     resolves the world and characters databases at runtime from worldserver.conf
--     (WorldDatabaseInfo / CharacterDatabaseInfo, the fifth ';' separated field).
--     The DEFAULT names are spelled out below only because a helper file has to
--     write something down.
--
-- On an install with non-default names, make a local copy with the names
-- substituted (use the schema names from your own worldserver.conf):
--
--   pwsh: (Get-Content useful_queries.sql) -replace 'acore_world','<world_db>' -replace 'acore_characters','<characters_db>' | Set-Content useful_queries.local.sql
--   bash: sed -e 's/acore_world/<world_db>/g' -e 's/acore_characters/<characters_db>/g' useful_queries.sql > useful_queries.local.sql
--
-- (useful_queries.local.sql is gitignored, so a substituted copy with real schema
-- names never lands in the repository.)
-- ---------------------------------------------------------------------------


-- ---------------------------------------------------------------------------
-- 1) Look an item up by (partial) name, with its class/subclass names
-- ---------------------------------------------------------------------------
SELECT
    it.entry
    , it.name
    , aic.name AS classname
    , aic2.name AS subclassname
    , aiq.name AS qualityname
    , it.RequiredLevel
    , it.BuyPrice
    , it.SellPrice
    , it.stackable
    , it.bonding
FROM
    acore_world.item_template it
LEFT JOIN acore_world.mod_auctionator_item_class aic ON it.class = aic.class AND aic.subclass IS NULL
LEFT JOIN acore_world.mod_auctionator_item_class aic2 ON it.class = aic2.class AND it.subclass = aic2.subclass
LEFT JOIN acore_world.mod_auctionator_item_quality aiq ON it.Quality = aiq.quality
WHERE
    it.name LIKE '%turalyon%'
LIMIT 1000;


-- ---------------------------------------------------------------------------
-- 2) What is in the auction house right now, by item
-- ---------------------------------------------------------------------------
SELECT
    it.name
    , it.class
    , it.subclass
    , ah.houseId
    , COUNT(*) AS auctions
    , MIN(ah.buyoutprice) AS min_buyout
    , MAX(ah.buyoutprice) AS max_buyout
FROM acore_characters.auctionhouse ah
INNER JOIN acore_characters.item_instance ii ON ah.itemguid = ii.guid
INNER JOIN acore_world.item_template it ON ii.itemEntry = it.entry
GROUP BY it.entry, it.name, it.class, it.subclass, ah.houseId
ORDER BY auctions DESC
LIMIT 1000;


-- ---------------------------------------------------------------------------
-- 3) The seller's candidate pool, exactly as AuctionatorSeller.cpp builds it
--    (itemclass_config joined with item_template, minus disabled_items), with the
--    live house count and the newest market scan next to it.
--    ONLY_FULL_GROUP_BY safe: the newest scan is selected through a join, not
--    through a bare column next to MAX().
-- ---------------------------------------------------------------------------
SELECT
    it.entry
    , it.name
    , it.class
    , it.subclass
    , it.bonding
    , it.BuyPrice
    , it.SellPrice
    , it.stackable
    , aicconf.bonding        AS cfg_bonding
    , aicconf.max_count      AS cfg_max_count
    , aicconf.stack_count    AS cfg_stack_count
    , ic.itemCount           AS auctions_in_house
    , mp.average_price       AS newest_market_average
    , mp.scan_datetime       AS newest_market_scan
FROM
    acore_world.mod_auctionator_itemclass_config aicconf
    INNER JOIN acore_world.item_template it ON
        aicconf.class = it.class
        AND aicconf.subclass = it.subclass
        AND it.bonding != 1
        AND (aicconf.bonding = 0 OR it.bonding >= aicconf.bonding)
        -- uncomment to mirror Auctionator.Seller.ExcludeUnverifiedItems = 1:
        -- AND it.VerifiedBuild != 1
    LEFT JOIN acore_world.mod_auctionator_disabled_items dis ON dis.item = it.entry
    LEFT JOIN (
        SELECT ii.itemEntry AS itemEntry, COUNT(*) AS itemCount
        FROM acore_characters.item_instance ii
        INNER JOIN acore_characters.auctionhouse ah ON ah.itemguid = ii.guid
        WHERE ah.houseId = 7
        GROUP BY ii.itemEntry
    ) ic ON ic.itemEntry = it.entry
    LEFT JOIN (
        SELECT mp1.entry, mp1.average_price, mp1.scan_datetime
        FROM acore_characters.mod_auctionator_market_price mp1
        INNER JOIN (
            SELECT entry, MAX(scan_datetime) AS max_scan
            FROM acore_characters.mod_auctionator_market_price
            GROUP BY entry
        ) mp2 ON mp2.entry = mp1.entry AND mp2.max_scan = mp1.scan_datetime
    ) mp ON mp.entry = it.entry
WHERE
    dis.item IS NULL
ORDER BY it.class, it.subclass, it.entry
LIMIT 1000;


-- ---------------------------------------------------------------------------
-- 4) How many items per class/subclass are eligible, and what the config says
--    (a quick way to spot a class that max_count = 0 has switched off)
-- ---------------------------------------------------------------------------
SELECT
    aicconf.class
    , aicconf.subclass
    , aicconf.bonding     AS cfg_bonding
    , aicconf.max_count   AS cfg_max_count
    , aicconf.stack_count AS cfg_stack_count
    , COUNT(*)            AS eligible_items
FROM acore_world.mod_auctionator_itemclass_config aicconf
INNER JOIN acore_world.item_template it ON
    aicconf.class = it.class
    AND aicconf.subclass = it.subclass
    AND it.bonding != 1
    AND (aicconf.bonding = 0 OR it.bonding >= aicconf.bonding)
    -- uncomment to mirror Auctionator.Seller.ExcludeUnverifiedItems = 1:
    -- AND it.VerifiedBuild != 1
LEFT JOIN acore_world.mod_auctionator_disabled_items dis ON dis.item = it.entry
WHERE dis.item IS NULL
GROUP BY aicconf.class, aicconf.subclass, aicconf.bonding, aicconf.max_count, aicconf.stack_count
ORDER BY eligible_items DESC;


-- ---------------------------------------------------------------------------
-- 5) Which eligible items are not in the auction house yet (restock candidates)
-- ---------------------------------------------------------------------------
SELECT it.entry, it.name, it.BuyPrice, it.SellPrice
FROM acore_world.item_template it
INNER JOIN acore_world.mod_auctionator_itemclass_config aicconf ON
    aicconf.class = it.class
    AND aicconf.subclass = it.subclass
    AND it.bonding != 1
    AND (aicconf.bonding = 0 OR it.bonding >= aicconf.bonding)
    -- uncomment to mirror Auctionator.Seller.ExcludeUnverifiedItems = 1:
    -- AND it.VerifiedBuild != 1
LEFT JOIN acore_world.mod_auctionator_disabled_items dis ON dis.item = it.entry
WHERE
    dis.item IS NULL
    AND it.entry NOT IN (
        SELECT ii.itemEntry
        FROM acore_characters.item_instance ii
        INNER JOIN acore_characters.auctionhouse ah ON ah.itemguid = ii.guid
    )
ORDER BY it.entry
LIMIT 1000;


-- ---------------------------------------------------------------------------
-- 6) Market price coverage: newest scan per item, newest/oldest timestamps
-- ---------------------------------------------------------------------------
SELECT
    mp1.entry
    , it.name
    , mp1.average_price
    , mp1.`count`
    , mp1.scan_datetime
    , mp1.source
    , mp1.imported_at
FROM acore_characters.mod_auctionator_market_price mp1
INNER JOIN (
    SELECT entry, MAX(scan_datetime) AS max_scan
    FROM acore_characters.mod_auctionator_market_price
    GROUP BY entry
) mp2 ON mp2.entry = mp1.entry AND mp2.max_scan = mp1.scan_datetime
LEFT JOIN acore_world.item_template it ON it.entry = mp1.entry
ORDER BY mp1.scan_datetime DESC
LIMIT 1000;

-- Coverage summary (how many items have a scan, and how fresh they are)
SELECT
    COUNT(*)                                        AS total_rows
    , COUNT(DISTINCT entry)                         AS distinct_items
    , MIN(scan_datetime)                            AS oldest_scan
    , MAX(scan_datetime)                            AS newest_scan
    , SUM(CASE WHEN scan_datetime >= NOW() - INTERVAL 14 DAY THEN 1 ELSE 0 END) AS rows_newer_than_14d
FROM acore_characters.mod_auctionator_market_price;


-- ---------------------------------------------------------------------------
-- 7) Market prices from THIS realm's own auction house.
--
--    This is the statement ".auctionator marketscan" runs (the module keeps the
--    canonical copy in AuctionatorMarketData::ScanAuctionHouse); it is repeated here
--    for people who would rather run it from cron or the mysql client, with
--    Auctionator.MarketData.ScanIntervalMinutes left at 0.
--
--    Characters database only: auctionhouse, item_instance and the market table all
--    live there. Prices in `auctionhouse` are for the whole stack while the market
--    table wants a per-unit price, hence the division by item_instance.count.
--    Replace 2 with Auctionator.CharacterGuid to leave the bot's own listings out of
--    the sample (what ScanExcludeSelf = 1 does); delete that one line to sample
--    everything, which is what a house holding only the bot's stock needs.
-- ---------------------------------------------------------------------------
INSERT INTO acore_characters.mod_auctionator_market_price
    (entry, average_price, buyout, bid, `count`, scan_datetime, source)
SELECT
    s.entry
    , LEAST(2147483647, GREATEST(1, s.average_price))
    , LEAST(2147483647, s.min_buyout)
    , LEAST(2147483647, s.min_bid)
    , s.listings
    , NOW()
    , 'ah-scan'
FROM (
    SELECT
        ii.itemEntry AS entry
        -- Volume weighted per-unit price: buyouts preferred, start bids only for items
        -- whose listings carry no buyout at all.
        , COALESCE(
            ROUND(SUM(CASE WHEN ah.buyoutprice > 0 THEN ah.buyoutprice END)
                  / NULLIF(SUM(CASE WHEN ah.buyoutprice > 0 THEN ii.count END), 0))
            , ROUND(SUM(CASE WHEN ah.startbid > 0 THEN ah.startbid END)
                  / NULLIF(SUM(CASE WHEN ah.startbid > 0 THEN ii.count END), 0))
            , 0) AS average_price
        , COALESCE(
            MIN(CASE WHEN ah.buyoutprice > 0 THEN ROUND(ah.buyoutprice / ii.count) END)
            , MIN(CASE WHEN ah.startbid > 0 THEN ROUND(ah.startbid / ii.count) END)
            , 0) AS min_buyout
        , COALESCE(MIN(CASE WHEN ah.startbid > 0 THEN ROUND(ah.startbid / ii.count) END), 0) AS min_bid
        , COUNT(*) AS listings
    FROM acore_characters.auctionhouse ah
    INNER JOIN acore_characters.item_instance ii ON ii.guid = ah.itemguid
    WHERE ii.itemEntry > 0
      AND ii.count > 0
      AND ah.itemowner <> 2      -- Auctionator.CharacterGuid: ScanExcludeSelf = 1
    GROUP BY ii.itemEntry
) s
WHERE s.average_price >= 1;

-- How many rows the scan above just wrote (same timestamp, same source).
SELECT COUNT(*) AS rows_written, COALESCE(SUM(`count`), 0) AS listings_sampled
FROM acore_characters.mod_auctionator_market_price
WHERE source = 'ah-scan' AND scan_datetime = (SELECT MAX(scan_datetime) FROM acore_characters.mod_auctionator_market_price WHERE source = 'ah-scan');


-- ---------------------------------------------------------------------------
-- 8) Investigate the bot's mailbox (should normally stay empty: the mail script
--    recycles auction mail for the configured auctionator character)
-- ---------------------------------------------------------------------------
SELECT
    m.id
    , m.sender
    , m.receiver
    , m.messageType
    , m.subject
    , it.name AS item_name
    , ii.count AS item_count
    , m.money
    , m.deliver_time
FROM acore_characters.mail m
LEFT JOIN acore_characters.mail_items mi ON mi.mail_id = m.id
LEFT JOIN acore_characters.item_instance ii ON mi.item_guid = ii.guid
LEFT JOIN acore_world.item_template it ON ii.itemEntry = it.entry
WHERE m.receiver = 2      -- Auctionator.CharacterGuid
ORDER BY m.id DESC
LIMIT 200;


-- ---------------------------------------------------------------------------
-- 9) Auction house totals per house (the same number .auctionator status shows)
-- ---------------------------------------------------------------------------
SELECT ah.houseId, COUNT(*) AS auctions
FROM acore_characters.auctionhouse ah
GROUP BY ah.houseId
ORDER BY ah.houseId;


-- ---------------------------------------------------------------------------
-- 10) Sale log: who bought what the module listed, and for how much.
--
--     One row per SOLD module listing (the automatic seller plus the GM add /
--     addlist commands), written by the module's "auction successful" hook at
--     every price level - unlike the core's own acore_characters.log_money, which
--     only gets a row for sales of 500 gold and up. Player-to-player auctions are
--     not recorded. seller_is_bot = 1 means the sale gold left the economy (it was
--     swallowed by the mail script), is_buyout = 1 means the listing was bought out
--     rather than won by bidding.
-- ---------------------------------------------------------------------------
SELECT
    s.id
    , s.sold_at
    , s.item_entry
    , it.name AS item_name
    , s.item_count
    , s.house_id
    , s.price
    , s.is_buyout
    , s.seller_guid
    , cs.name AS seller_name
    , s.seller_is_bot
    , s.buyer_guid
    , cb.name AS buyer_name
    , s.startbid
    , s.buyout
    , s.cut
FROM acore_characters.mod_auctionator_sale s
LEFT JOIN acore_world.item_template it ON it.entry = s.item_entry
LEFT JOIN acore_characters.characters cs ON cs.guid = s.seller_guid
LEFT JOIN acore_characters.characters cb ON cb.guid = s.buyer_guid
ORDER BY s.sold_at DESC, s.id DESC
LIMIT 200;

-- Sales summary per day (gold out of the economy, how much of it was bought out)
SELECT
    DATE(s.sold_at)                       AS day
    , COUNT(*)                            AS sales
    , SUM(s.is_buyout)                    AS buyouts
    , SUM(s.price)                        AS copper_total
    , SUM(s.price) / 10000                AS gold_total
    , COUNT(DISTINCT s.buyer_guid)        AS distinct_buyers
FROM acore_characters.mod_auctionator_sale s
WHERE s.seller_is_bot = 1
GROUP BY DATE(s.sold_at)
ORDER BY day DESC
LIMIT 60;

-- The most valuable sales, to spot a mispriced listing
SELECT s.sold_at, s.item_entry, it.name, s.item_count, s.price, s.is_buyout, cb.name AS buyer
FROM acore_characters.mod_auctionator_sale s
LEFT JOIN acore_world.item_template it ON it.entry = s.item_entry
LEFT JOIN acore_characters.characters cb ON cb.guid = s.buyer_guid
ORDER BY s.price DESC
LIMIT 50;


-- ---------------------------------------------------------------------------
-- OPERATING (DESTRUCTIVE) - commented out on purpose, read before uncommenting.
-- ---------------------------------------------------------------------------

-- Reset the auction house (SERVER MUST BE STOPPED: the AH is cached in memory).
-- Also documented in the README.
-- DELETE FROM acore_characters.item_instance WHERE guid IN (SELECT itemguid FROM acore_characters.auctionhouse);
-- DELETE FROM acore_characters.auctionhouse;

-- Drop the bot's queued mail (only for the auctionator character's guid).
-- DELETE FROM acore_characters.mail_items WHERE receiver = 2;
-- DELETE FROM acore_characters.mail WHERE receiver = 2;

-- Re-seed mod_auctionator_itemclass_config from scratch. This DELETES the table,
-- so every tuning a GM made in it is lost - use the idempotent SQL update
-- (data/sql/db-world/base/2023-09-18.sql) instead if you only want the defaults.
-- DROP TABLE IF EXISTS acore_world.mod_auctionator_itemclass_config;
-- (then re-apply data/sql/db-world/base/2023-09-18.sql)
