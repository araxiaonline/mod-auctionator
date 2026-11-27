
#ifndef AUCTIONATORBIDDER_H
#define AUCTIONATORBIDDER_H

#include "Auctionator.h"
#include "AuctionatorBase.h"
#include "ObjectMgr.h"
#include <cstddef>
#include <unordered_map>
#include <vector>

class AuctionatorBidder : public AuctionatorBase
{
    private:
        // Newest market scan of one item, preloaded once per cycle (see LoadMarketData).
        struct MarketScan
        {
            uint32 averagePrice = 0;
            // Age of the scan in seconds as measured by the database (see AuctionatorSeller).
            uint32 ageSeconds = 0;
        };

        uint32 auctionHouseId;
        AuctionHouseObject* ahMgr;
        ObjectGuid buyerGuid;
        AuctionatorConfig* config;
        std::unordered_map<uint32, MarketScan> marketData;
        uint32 GetAuctionsPerCycle() const;
        // One query for the whole cycle instead of one per priced auction.
        void LoadMarketData(std::vector<uint32>& itemEntries);
        // The module holds no money: every bid/buyout it records is unfunded while the
        // core still pays the seller, so each purchase creates gold. Logged on every
        // purchase on purpose (see the README economy section).
        void LogUnfundedPurchase(char const* action, AuctionEntry const* auction, uint32 amount);

    public:
        AuctionatorBidder(uint32 auctionHouseIdParam, ObjectGuid buyer, AuctionatorConfig* auctionatorConfig);
        void SpendSomeCash();
        // Consumes the id at nextIndex (advancing the cursor) and resolves it against the
        // in-memory auction house. A cursor instead of erase(begin()) keeps the walk over
        // the sampled window linear.
        AuctionEntry* GetAuctionForPurchase(std::vector<uint32> const& biddableAuctionIds, std::size_t& nextIndex);

        bool BidOnAuction(AuctionEntry* auction, ItemTemplate const* itemTemplate);
        bool BuyoutAuction(AuctionEntry* auction, ItemTemplate const* itemTemplate);
        uint32 CalculateBuyPrice(AuctionEntry const* auction, ItemTemplate const* item) const;
 };

#endif  //AUCTIONATORBIDDER_H
