--
-- Indexes used by the auctionator seller:
--   auctionhouse(houseId, itemguid) - "how many auctions of item X are in house Y"
--   item_instance(guid, itemEntry)  - covering lookup for the same query
--
-- Module SQL updates run against the characters database, so the tables are NOT
-- schema-qualified: a hardcoded `acore_characters.` prefix breaks every install
-- that uses a different database name (and the updater then refuses to start the
-- worldserver).
--
-- Each index is created only when it does not exist yet. The AzerothCore updater
-- re-applies an update file whose hash changed ("Reapplying update ... (it
-- changed)"), so this file must stay idempotent.
--

SET @hasAhIndex = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`STATISTICS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'auctionhouse' AND `INDEX_NAME` = 'idx_ah_houseid_itemguid');
SET @sql = IF(@hasAhIndex = 0, 'CREATE INDEX `idx_ah_houseid_itemguid` ON `auctionhouse` (`houseId`, `itemguid`)', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @hasIiIndex = (SELECT COUNT(*) FROM `INFORMATION_SCHEMA`.`STATISTICS` WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'item_instance' AND `INDEX_NAME` = 'idx_ii_guid_entry');
SET @sql = IF(@hasIiIndex = 0, 'CREATE INDEX `idx_ii_guid_entry` ON `item_instance` (`guid`, `itemEntry`)', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
