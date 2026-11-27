--
-- GM curated listing list for mod-auctionator.
--
-- Rows of this table are listed by ".auctionator addlist [house] [owner]".
--
-- `mode` says how the row is priced:
--   legacy : follow the realm-wide Auctionator.Seller.BidOnly / BidStartModifier
--            pair (the behaviour every row had before `mode` existed, and the
--            column default, so an existing roster is unaffected by the upgrade).
--   buyout : one fixed price. `price` is the buyout and the start bid is pinned to
--            it, so the stack cannot be won with a low bid that then expires.
--   bid    : auction. `bid` is the start bid and is mandatory; `price` is the
--            optional buyout (0 = none) and may not be below the start bid.
--
-- `price` and `bid` are UNIT prices in copper (per single item): the listing price
-- is the unit price times the stack, capped at the maximum money amount.
-- owner = 0 means the configured auctionator character, i.e. the sale money is
-- recycled by the system (gold sink) - see mod_auctionator.conf.dist.
--

--
-- This file lives in `base/`, but the AzerothCore updater registers every
-- `modules/<mod>/data/sql/db-<db>/` directory (base included) as a MODULE update and
-- RE-RUNS the whole file whenever its hash changes. It therefore has to stay
-- idempotent: CREATE TABLE IF NOT EXISTS + INSERT IGNORE, so the rows a GM curated
-- (or enabled) are never dropped or overwritten.
--
-- Existing tables get `mode` and `bid` from the guarded ALTERs in
-- `updates/2026_09_24_00_gm_list_mode.sql`, which this definition mirrors for fresh
-- installs.
--

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

--
-- Example rows (kept disabled so a fresh install lists nothing by accident).
-- Adjust the prices for your realm and set enabled = 1.
--
INSERT IGNORE INTO `mod_auctionator_gm_list`
  (`item`, `mode`, `price`, `bid`, `stack`, `hours`, `house`, `owner`, `enabled`) VALUES
(5500, 'buyout', 10000, 0, 1, 48, 7, 0, 0),   -- Iridescent Pearl: one fixed price
(4359, 'bid', 0, 500, 5, 48, 7, 0, 0);         -- Handful of Copper Bolts: 500c start bid per unit, no buyout
