--
-- Per-quality gate for the mod-auctionator automatic seller.
--
-- Before this update the seller's candidate query only joined
-- `mod_auctionator_itemclass_config` (the class/subclass whitelist), so quality could not be
-- used to decide what gets listed at all - it only picked the price multiplier. A GM asking
-- for "never list poor items" or "only rare and above" had no way to express it.
--
--   `quality` : item_template.quality - 0 poor .. 7 heirloom
--   `enabled` : 1 = the automatic seller may list this quality, 0 = it never does
--
-- A quality without a row is ALLOWED on purpose: an empty table therefore changes nothing,
-- and a world database that has not been touched keeps listing exactly what it used to. The
-- shipped rows state that default explicitly so the panel opens on a meaningful state.
--
-- CREATE TABLE IF NOT EXISTS + INSERT IGNORE make this file idempotent, so running it on an
-- already migrated database is harmless and never overwrites a choice a GM made.
--

CREATE TABLE IF NOT EXISTS `mod_auctionator_quality_config` (
  `quality` tinyint unsigned NOT NULL COMMENT 'item_template.quality: 0 poor .. 7 heirloom',
  `enabled` tinyint NOT NULL DEFAULT '1' COMMENT '1 to let the automatic seller list this quality, 0 to never list it',
  PRIMARY KEY (`quality`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- "Everything allowed" is the behaviour a database without this table already had.
INSERT IGNORE INTO `mod_auctionator_quality_config` (`quality`, `enabled`) VALUES
(0, 1),  -- poor
(1, 1),  -- normal
(2, 1),  -- uncommon
(3, 1),  -- rare
(4, 1),  -- epic
(5, 1),  -- legendary
(6, 1),  -- artifact
(7, 1);  -- heirloom
