
#ifndef AUCTIONATORSELLER_H
#define AUCTIONATORSELLER_H

#include "Auctionator.h"
#include "AuctionHouseMgr.h"

struct CachedItem {
    uint32 entry = 0;
    std::string name;
    uint32 basePrice = 0;         // item_template.BuyPrice (vendor buy price)
    uint32 sellPrice = 0;         // item_template.SellPrice (vendor sell price, price floor reference)
    uint32 stackable = 0;
    uint32 quality = 0;
    uint32 marketPrice = 0;       // mod_auctionator_market_price.average_price (0 = no data)
    uint32 marketCount = 0;       // mod_auctionator_market_price.count (traded volume, 0 = unknown)
    // Age of the newest scan in seconds, measured by the database against its own clock
    // (0 = no timestamp). Comparing it to a C++ epoch would mix two clocks: the stored
    // scan_datetime is interpreted in the database session's time zone.
    uint32 marketAgeSeconds = 0;
    uint32 maxCount = 0;
    uint32 stackCount = 0;        // mod_auctionator_itemclass_config.stack_count (0 = use the item's max stack)
    // Effective item level used by the Auctionator.Seller.MaxItemLevel gate: the item's own
    // ItemLevel, or - for a recipe - the level of the item its craft spell creates.
    uint32 itemLevel = 0;
};

class AuctionatorSeller : public AuctionatorBase
{
    private:
        Auctionator* nator;

    public:
        explicit AuctionatorSeller(Auctionator* natorParam);
        void LetsGetToIt(uint32 maxCount, uint32 houseId);
        static uint32 GetRandomNumber(uint32 min, uint32 max);
};

#endif  //AUCTIONATORSELLER_H
