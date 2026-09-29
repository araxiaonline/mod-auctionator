--
-- Per-quality gate for the mod-auctionator automatic seller.
--
-- The seller picks its candidates with one query that already joins
-- `mod_auctionator_itemclass_config` (the class/subclass whitelist, where `max_count = 0`
-- means "never list this class"). Quality had no gate at all: it only picked the price
-- multiplier, so "never list poor items" could not be expressed.
--
--   quality : item_template.quality - 0 poor, 1 normal, 2 uncommon, 3 rare, 4 epic,
--             5 legendary, 6 artifact, 7 heirloom
--   enabled : 1 = the seller may list this quality, 0 = it never does
--
-- A quality with NO row is allowed, so an empty table changes nothing and an older world
-- database keeps listing exactly what it used to. The shipped rows spell that default out.
--
-- The seller re-reads this table on every cycle (the candidate pool is refreshed per tick),
-- so a change here takes effect on the next run without a worldserver restart.
--

CREATE TABLE IF NOT EXISTS `mod_auctionator_quality_config` (
  `quality` tinyint unsigned NOT NULL COMMENT 'item_template.quality: 0 poor .. 7 heirloom',
  `enabled` tinyint NOT NULL DEFAULT '1' COMMENT '1 to let the automatic seller list this quality, 0 to never list it',
  PRIMARY KEY (`quality`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- Every quality allowed is the behaviour a database without this table already had.
INSERT IGNORE INTO `mod_auctionator_quality_config` (`quality`, `enabled`) VALUES
(0, 1),  -- poor
(1, 1),  -- normal
(2, 1),  -- uncommon
(3, 1),  -- rare
(4, 1),  -- epic
(5, 1),  -- legendary
(6, 1),  -- artifact
(7, 1);  -- heirloom
