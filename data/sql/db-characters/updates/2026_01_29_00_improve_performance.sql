CREATE INDEX `idx_itementry` ON `acore_characters`.`item_instance`(`itemEntry`);
CREATE INDEX `idx_ah_houseid_itemguid` ON `acore_characters`.`auctionhouse`(`houseId`, `itemguid`);
