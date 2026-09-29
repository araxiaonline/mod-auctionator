--
-- Provenance for the listings this module created, in the characters database:
--
--   mod_auctionator_auction(auction_id, owner_guid, item_entry, created_at)
--
-- Why it exists: the sale log used to answer "is this listing mine?" from `deposit = 0`, on the
-- assumption that the core always charges a player a deposit. It does not - see
-- AuctionHouseMgr::GetAuctionDeposit(), which computes
--
--   AH_MINIMUM_DEPOSIT * sWorld->getRate(RATE_AUCTION_DEPOSIT)
--
-- for an item with no vendor price, and uses the same value as a floor otherwise. On a realm
-- configured with `Rate.Auction.Deposit = 0` both branches yield 0, so every player listing also
-- carries deposit = 0, the marker stops proving anything, and mod_auctionator_sale fills up with
-- player-to-player sales. This table removes the guess: the module writes the id of every auction
-- it creates, and the sale log asks it instead.
--
-- Columns:
--   auction_id   `auctionhouse.id` the module generated (sObjectMgr->GenerateAuctionID())
--   owner_guid   the owner it was created for: the configured Auctionator character, or the
--                explicit `owner=` of `.auctionator add` / the panel's 指定角色 field
--   item_entry   `item_template.entry`, for diagnostics
--   created_at   server local time
--
-- Rows are append-only and never deleted. An auction id is never reused within a realm
-- (GenerateAuctionID() is max+1), so a stale row is harmless and keeps already-recorded sales
-- attributable. Prune by age from your own cron if the size ever matters:
--   DELETE FROM mod_auctionator_auction WHERE created_at < NOW() - INTERVAL 180 DAY;
--
-- The second statement records the answer on the sale row itself, so nothing downstream has to
-- infer it again:
--   mod_auctionator_sale.module_listing
--       1    the registry knew this auction: the module created it
--       0    it did not - a listing owned by the configured character but created outside the
--            module (a human playing it, or a build from before this table existed)
--       NULL the row was written before this column existed, so its provenance is unknown
--
-- Idempotent: both statements are guarded with information_schema, so re-running this file (or
-- letting the DB updater apply it on every start) is harmless.
--

SET @tableExists = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`TABLES` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_auction');
SET @sql = IF(@tableExists = 0, 'CREATE TABLE `mod_auctionator_auction` (`auction_id` INT UNSIGNED NOT NULL, `owner_guid` INT UNSIGNED NOT NULL, `item_entry` INT UNSIGNED NOT NULL DEFAULT 0, `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (`auction_id`), KEY `idx_owner` (`owner_guid`), KEY `idx_created_at` (`created_at`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @columnExists = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_sale' AND `COLUMN_NAME` = 'module_listing');
SET @sql = IF(@columnExists = 0, 'ALTER TABLE `mod_auctionator_sale` ADD COLUMN `module_listing` TINYINT UNSIGNED NULL DEFAULT NULL AFTER `seller_is_bot`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
