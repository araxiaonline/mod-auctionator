#include "Auctionator.h"
#include "AuctionHouseMgr.h"
#include "AuctionatorSeller.h"
#include "Item.h"
#include "DatabaseEnv.h"
#include "PreparedStatement.h"
#include <random>
#include "QueryResult.h"


AuctionatorSeller::AuctionatorSeller(Auctionator* natorParam, uint32 auctionHouseIdParam)
{
    SetLogPrefix("[AuctionatorSeller] ");
    nator = natorParam;
    auctionHouseId = auctionHouseIdParam;

    ahMgr = nator->GetAuctionMgr(auctionHouseId);
};

AuctionatorSeller::~AuctionatorSeller()
{
    // TODO: clean up
};

void AuctionatorSeller::LetsGetToIt(uint32 maxCount, uint32 houseId)
{
    std::string characterDbName = CharacterDatabase.GetConnectionInfo()->database;
    static std::vector<CachedItem> cachedItems = []() {
        std::vector<CachedItem> items;

        std::string cacheQuery = R"(
            SELECT
                it.entry, it.name, it.BuyPrice, it.stackable, it.quality
                , COALESCE(mp.average_price, 0) as average_price, aicconf.max_count
            FROM
                mod_auctionator_itemclass_config aicconf
                INNER JOIN item_template it ON
                    aicconf.class = it.class
                    AND aicconf.subclass = it.subclass
                    AND it.bonding != 1
                    AND (it.bonding >= aicconf.bonding OR it.bonding = 0)
                    AND it.VerifiedBuild != 1
                LEFT JOIN mod_auctionator_disabled_items dis ON it.entry = dis.item
                LEFT JOIN (
                    SELECT mp1.entry, mp1.average_price
                    FROM {}.mod_auctionator_market_price mp1
                    INNER JOIN (
                        SELECT entry, MAX(scan_datetime) as max_scan
                        FROM {}.mod_auctionator_market_price
                        GROUP BY entry
                    ) mp2 ON mp1.entry = mp2.entry AND mp1.scan_datetime = mp2.max_scan
                ) mp ON it.entry = mp.entry
            WHERE dis.item IS NULL
        )";

        QueryResult result = WorldDatabase.Query(cacheQuery, characterDbName, characterDbName);

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                CachedItem item;
                item.entry = fields[0].Get<uint32>();
                item.name = fields[1].Get<std::string>();
                item.basePrice = fields[2].Get<uint32>();
                item.stackable = fields[3].Get<uint32>();
                item.quality = fields[4].Get<uint32>();
                item.marketPrice = fields[5].Get<uint32>();
                item.maxCount = fields[6].Get<uint32>();
                items.push_back(item);
            } while (result->NextRow());
        }

        return items;
    }();


    std::string countQuery = R"(
        SELECT ii.itemEntry, COUNT(*) as itemCount
        FROM {}.item_instance ii
        INNER JOIN {}.auctionhouse ah ON ii.guid = ah.itemguid
        WHERE ah.houseId = {}
        GROUP BY ii.itemEntry
    )";

    QueryResult countResult = CharacterDatabase.Query(countQuery, characterDbName, characterDbName, houseId);

    std::unordered_map<uint32, uint32> currentCounts;
    if (countResult)
    {
        do
        {
            Field* fields = countResult->Fetch();
            currentCounts[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (countResult->NextRow());
    }

    std::vector<CachedItem> shuffled = cachedItems;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(shuffled.begin(), shuffled.end(), gen);

    uint32 count = 0;
    for (const auto& item : shuffled)
    {
        uint32 currentCount = currentCounts[item.entry];
        if (currentCount >= item.maxCount) continue;

        std::string itemName = item.name;

        uint32 stackSize = item.stackable;
        if (stackSize > 20) {
            stackSize = 20;
        }

        if (stackSize > 1 && nator->config->sellerConfig.randomizeStackSize) {
            stackSize = GetRandomNumber(1, stackSize);
            logDebug("Stack size: " + std::to_string(stackSize));
        }

        float qualityMultiplier = Auctionator::GetQualityMultiplier(nator->config->sellerMultipliers, item.quality);

        uint32 price = item.marketPrice > 0 ? item.marketPrice : item.basePrice;
        if (item.marketPrice > 0) {
            logDebug("Using Market over Template [" + itemName + "] " +
                std::to_string(item.marketPrice) + " <--> " + std::to_string(item.basePrice));
        }

        if (price == 0) {
            price = 10000000 * qualityMultiplier;
        }

        uint32 bidPrice = price;
        float bidStartModifier = nator->config->sellerConfig.bidStartModifier;
        logDebug("Bid start modifier: " + std::to_string(bidStartModifier));
        bidPrice = GetRandomNumber(bidPrice - (bidPrice * bidStartModifier), bidPrice);
        logDebug("Bid price " + std::to_string(bidPrice) + " from price " + std::to_string(price));

        AuctionatorItem newItem = AuctionatorItem();
        newItem.itemId = item.entry;
        newItem.quantity = 1;
        newItem.houseId = houseId;
        newItem.buyout = uint32(price * stackSize * qualityMultiplier);
        newItem.bid = uint32(bidPrice * stackSize * qualityMultiplier);
        newItem.time = 60 * 60 * 12;
        newItem.stackSize = stackSize;

        logDebug("Adding item: " + itemName
            + " with quantity of " + std::to_string(newItem.quantity)
            + " at price of " +  std::to_string(newItem.buyout)
            + " to house " + std::to_string(houseId)
        );

        nator->CreateAuction(newItem);

        count++;
        if (count == maxCount) {
            break;
        }
    }

    logInfo("Items added houseId("
        + std::to_string(houseId)
        + ") this run: " + std::to_string(count));

};

uint32 AuctionatorSeller::GetRandomNumber(uint32 min, uint32 max)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(min, max);
    return dis(gen);
}
