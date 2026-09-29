--
-- Explicit listing mode + start bid for mod-auctionator's GM listing paths.
--
-- Before this update a mod_auctionator_gm_list row carried a single `price`, and what
-- that price meant was decided realm-wide by Auctionator.Seller.BidOnly: either a
-- buyout with a BidStartModifier-discounted start bid, or a pure-auction start bid.
-- A GM could therefore not list one item at a fixed price and the next as an auction,
-- and the start bid was never theirs to choose.
--
--   `mode` : 'legacy' (default, follow Auctionator.Seller.BidOnly exactly as before),
--            'buyout' (one fixed price; the start bid is pinned to the buyout so the
--            stack cannot be won with a low bid that then expires), or
--            'bid' (auction: `bid` is the start bid, `price` the optional buyout).
--   `bid`  : unit start bid in copper, used by mode='bid' only.
--
-- The default is 'legacy' on purpose: rows that already exist keep their old meaning
-- until a GM edits them, so upgrading changes nothing that is already listed. There is
-- deliberately no UPDATE here - re-running the file must never overwrite a mode a GM
-- has chosen.
--
-- Every statement is guarded with information_schema, so running this file on an
-- already migrated database is harmless.
--

-- A database that never had the table at all gets the current definition in one go.
CREATE TABLE IF NOT EXISTS `mod_auctionator_gm_list` (
  `item` int unsigned NOT NULL COMMENT 'item_template.entry',
  `mode` varchar(8) NOT NULL DEFAULT 'legacy' COMMENT 'legacy = follow Auctionator.Seller.BidOnly; buyout = one fixed price; bid = auction with an explicit start bid',
  `price` int unsigned NOT NULL DEFAULT '0' COMMENT 'unit buyout price in copper (buyout mode: required; bid mode: optional, 0 = none); listing buyout = price * stack',
  `bid` int unsigned NOT NULL DEFAULT '0' COMMENT 'unit start bid in copper for mode = bid (required there, ignored by the other modes); listing start bid = bid * stack',
  `stack` int unsigned NOT NULL DEFAULT '1' COMMENT 'stack size, capped by the item max stack',
  `hours` int unsigned NOT NULL DEFAULT '48' COMMENT 'auction duration in hours (1..720)',
  `house` int unsigned NOT NULL DEFAULT '7' COMMENT '2 = alliance, 6 = horde, 7 = neutral',
  `owner` int unsigned NOT NULL DEFAULT '0' COMMENT 'character guid receiving the sale money; 0 = the configured auctionator character (gold recycled)',
  `enabled` tinyint NOT NULL DEFAULT '1' COMMENT '0 to keep the row in the table but skip it',
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

SET @hasMode = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_gm_list' AND `COLUMN_NAME` = 'mode');
SET @sql = IF(@hasMode = 0, 'ALTER TABLE `mod_auctionator_gm_list` ADD COLUMN `mode` varchar(8) NOT NULL DEFAULT ''legacy'' COMMENT ''legacy = follow Auctionator.Seller.BidOnly; buyout = one fixed price; bid = auction with an explicit start bid'' AFTER `item`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @hasBid = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_gm_list' AND `COLUMN_NAME` = 'bid');
SET @sql = IF(@hasBid = 0, 'ALTER TABLE `mod_auctionator_gm_list` ADD COLUMN `bid` int unsigned NOT NULL DEFAULT ''0'' COMMENT ''unit start bid in copper for mode = bid (required there, ignored by the other modes); listing start bid = bid * stack'' AFTER `price`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Repair rows whose mode is unusable so ".auctionator addlist" does not have to skip
-- them: an empty or unknown mode is the legacy behaviour, and a legacy/buyout row never
-- has a start bid of its own. Only rows that are actually wrong are touched.
UPDATE `mod_auctionator_gm_list` SET `mode` = 'legacy' WHERE `mode` IS NULL OR `mode` NOT IN ('legacy', 'buyout', 'bid');
UPDATE `mod_auctionator_gm_list` SET `bid` = 0 WHERE `bid` IS NULL;
