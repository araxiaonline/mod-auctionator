CREATE INDEX idx_ah_houseid_itemguid ON acore_characters.auctionhouse(houseId, itemguid);
CREATE INDEX idx_ii_guid_entry ON acore_characters.item_instance(guid, itemEntry);

