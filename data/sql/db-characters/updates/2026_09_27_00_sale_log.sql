--
-- Sale log for mod-auctionator.
--
-- The core keeps no record of a *finished* auction:
--
--   * `characters.auctionhouse` only holds the live auctions - the row is deleted in the
--     same transaction that pays the seller (AuctionHouseObject::Update() for a winning
--     bid, WorldSession::HandleAuctionPlaceBid() for a buyout) - and
--   * the core's own `characters.log_money` row is written for sales of 500 gold and up
--     only (AuctionHouseMgr::SendAuctionSuccessfulMail()).
--
-- So a listing this module created (the automatic seller, `.auctionator add` and
-- `addlist`) left no trace at all once a player won it, at any price below that
-- threshold. This table is that trace: the module writes one row per sold
-- module-created listing, at every price level, from its "auction successful" hook.
--
-- Player-to-player auctions are NOT recorded: only listings the module created
-- (`deposit = 0`, see the README economy section) or that are owned by the configured
-- Auctionator character.
--
-- Columns:
--   auction_id     `auctionhouse.id` the sale came from (the row is gone by now)
--   item_entry     `item_template.entry`, item_count = how many were in the stack
--   house_id       2 = alliance, 6 = horde, 7 = neutral
--   seller_guid    the listing's owner
--   seller_is_bot  1 when the owner IS Auctionator.CharacterGuid, i.e. the sale gold was
--                  swallowed by the mail script (gold sink), 0 for an explicit owner
--   buyer_guid     the winner (the bidder the core settled with)
--   price          what the winner paid, for the whole stack, in copper
--   startbid       the listing's start bid / buyout / deposit at the time of the sale
--   buyout
--   deposit
--   cut            the auction house cut the core took out of the price
--   is_buyout      1 = bought out, 0 = won by bidding (price is then the winning bid)
--   sold_at        server local time, the same clock as `auctionhouse.time`
--
-- The module never updates or deletes rows here. To keep the table small, prune it from
-- your own cron, e.g.:
--   DELETE FROM mod_auctionator_sale WHERE sold_at < NOW() - INTERVAL 180 DAY;
--
-- Every money column is INT UNSIGNED: the core's bid/buyout/deposit/cut are uint32 values
-- capped at MAX_MONEY_AMOUNT, so they fit these columns exactly.
--
-- Idempotent: the CREATE is guarded with information_schema, so re-running this file (or
-- letting the DB updater apply it on every start) is harmless.
--

SET @tableExists = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`TABLES` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_sale');
SET @sql = IF(@tableExists = 0, 'CREATE TABLE `mod_auctionator_sale` (`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `auction_id` INT UNSIGNED NOT NULL, `item_entry` INT UNSIGNED NOT NULL, `item_count` INT UNSIGNED NOT NULL DEFAULT 1, `house_id` TINYINT UNSIGNED NOT NULL DEFAULT 7, `seller_guid` INT UNSIGNED NOT NULL, `seller_is_bot` TINYINT UNSIGNED NOT NULL DEFAULT 0, `buyer_guid` INT UNSIGNED NOT NULL, `price` INT UNSIGNED NOT NULL DEFAULT 0, `startbid` INT UNSIGNED NOT NULL DEFAULT 0, `buyout` INT UNSIGNED NOT NULL DEFAULT 0, `deposit` INT UNSIGNED NOT NULL DEFAULT 0, `cut` INT UNSIGNED NOT NULL DEFAULT 0, `is_buyout` TINYINT UNSIGNED NOT NULL DEFAULT 0, `sold_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (`id`), KEY `idx_sold_at` (`sold_at`), KEY `idx_item` (`item_entry`), KEY `idx_buyer` (`buyer_guid`), KEY `idx_seller` (`seller_guid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
