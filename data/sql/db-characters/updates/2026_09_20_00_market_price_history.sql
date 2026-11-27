--
-- Market price history support for mod-auctionator.
--
-- Before this update the table had PRIMARY KEY (`entry`), so it could hold exactly
-- one row per item: a second import failed with a duplicate key error, no history
-- was kept and the seller's "latest scan per item" subquery was pointless.
--
-- The primary key becomes (`entry`, `scan_datetime`) so that imports can upsert per
-- (item, scan) pair, older scans are retained (see ".auctionator marketprune"), and
-- a partial import can never wipe the prices of the items it does not mention.
--
-- A `source` column records where the data came from and `imported_at` when it was
-- written. Every statement below is guarded with information_schema, so running
-- this file on an already migrated database is harmless.
--

SET @tableExists = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`TABLES` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_market_price');
SET @sql = IF(@tableExists = 0, 'CREATE TABLE `mod_auctionator_market_price` (`entry` INT NOT NULL, `scan_datetime` DATETIME NOT NULL, `average_price` INT NOT NULL DEFAULT 0, `buyout` INT NOT NULL DEFAULT 0, `bid` INT NOT NULL DEFAULT 0, `count` INT NOT NULL DEFAULT 0, `source` VARCHAR(32) NOT NULL DEFAULT '''', `imported_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (`entry`, `scan_datetime`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Primary key columns must not be NULL: repair any pre-existing NULL scan before
-- touching the key, otherwise the ALTER below would fail and block server startup.
UPDATE `mod_auctionator_market_price` SET `scan_datetime` = NOW() WHERE `scan_datetime` IS NULL;
UPDATE `mod_auctionator_market_price` SET `entry` = 0 WHERE `entry` IS NULL;

SET @hasSource = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_market_price' AND `COLUMN_NAME` = 'source');
SET @sql = IF(@hasSource = 0, 'ALTER TABLE `mod_auctionator_market_price` ADD COLUMN `source` VARCHAR(32) NOT NULL DEFAULT '''' AFTER `count`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @hasImportedAt = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_market_price' AND `COLUMN_NAME` = 'imported_at');
SET @sql = IF(@hasImportedAt = 0, 'ALTER TABLE `mod_auctionator_market_price` ADD COLUMN `imported_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP AFTER `source`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- (entry) -> (entry, scan_datetime)
SET @pkHasScan = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`STATISTICS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_market_price' AND `INDEX_NAME` = 'PRIMARY' AND `COLUMN_NAME` = 'scan_datetime');
SET @sql = IF(@pkHasScan = 0, 'ALTER TABLE `mod_auctionator_market_price` DROP PRIMARY KEY, ADD PRIMARY KEY (`entry`, `scan_datetime`)', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Drop the redundant single-column index the old definition carried
-- (Index(`entry`, `scan_datetime`) was auto-named "entry").
SET @dupIndex = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`STATISTICS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mod_auctionator_market_price' AND `INDEX_NAME` = 'entry');
SET @sql = IF(@dupIndex > 0, 'ALTER TABLE `mod_auctionator_market_price` DROP INDEX `entry`', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
