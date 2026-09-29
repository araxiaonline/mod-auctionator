#include "AuctionatorBidder.h"
#include "Auctionator.h"
#include "AuctionHouseSearcher.h"
#include "ObjectMgr.h"
#include "DatabaseEnv.h"
// MAX_MONEY_AMOUNT is a macro in Player.h and is declared nowhere else, so this include is
// needed even though this file never touches a Player object.
#include "Player.h"
#include "ScriptMgr.h"
#include "QueryResult.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <random>
#include <string>

AuctionatorBidder::AuctionatorBidder(uint32 auctionHouseIdParam, ObjectGuid buyer, AuctionatorConfig* auctionatorConfig)
{
    SetLogPrefix("[AuctionatorBidder] ");
    auctionHouseId = auctionHouseIdParam;
    buyerGuid = buyer;
    config = auctionatorConfig;
    ahMgr = sAuctionMgr ? sAuctionMgr->GetAuctionsMapByHouseId((AuctionHouseId)auctionHouseId) : nullptr;
    if (!config) {
        logError("AuctionatorBidder constructed without a valid config pointer.");
    }
}

void AuctionatorBidder::SpendSomeCash()
{
    if (!config || !sAuctionMgr || !ahMgr) {
        logWarn("AuctionatorBidder::SpendSomeCash skipped: config or auction manager is unavailable.");
        return;
    }

    //
    // On a realm that shares one auction house between the factions, the core stores every
    // *player* auction with houseId = Neutral (WorldSession::HandleAuctionSellItem forces
    // it), so a bidder configured for Alliance/Horde would find no player auction at all -
    // only the module's own listings, which the owner filter excludes. Report that instead
    // of silently doing nothing every cycle.
    //
    if (auctionHouseId != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
    {
        logError("bidder skipped for house " + std::to_string(auctionHouseId)
            + ": AllowTwoSide.Interaction.Auction is enabled, so all player auctions live in house 7 (neutral). "
              "Enable Auctionator.NeutralBidder and leave the alliance/horde bidders disabled.");
        return;
    }

    // GetCounter() on purpose: GetRawValue() is 64 bit and would silently truncate.
    uint32 const auctionatorPlayerGuid = buyerGuid.GetCounter();

    uint32 const purchasesPerCycle = GetAuctionsPerCycle();
    // Saturating: MaxPerCycle is clamped when it is loaded, but the id window must never
    // be able to overflow the multiplication.
    uint64 const requestedWindow = static_cast<uint64>(purchasesPerCycle) * 10u;
    uint32 const maxQueryCount = static_cast<uint32>(std::min<uint64>(std::max<uint64>(requestedWindow, 100u), 100000u));

    // For testing we may want to bid on our own auctions: then the owner filter is
    // dropped so we pick up all auctions including our own.
    uint32 ownerToSkip = auctionatorPlayerGuid;
    if (config->bidOnOwn) {
        ownerToSkip = 0;
    }

    //
    // Take a random window over the house's id range instead of always its oldest rows:
    // "ORDER BY id LIMIT n" always queried the same lowest ids, i.e. the auctions closest
    // to expiry. Both queries are primary key range scans.
    //
    std::string rangeQuery = R"(
        SELECT MIN(id), MAX(id)
        FROM auctionhouse
        WHERE itemowner <> {} AND houseid = {} AND `time` > UNIX_TIMESTAMP()
    )";

    QueryResult rangeResult = CharacterDatabase.Query(rangeQuery, ownerToSkip, auctionHouseId);
    uint32 minId = 0;
    uint32 maxId = 0;
    if (rangeResult && rangeResult->GetRowCount() > 0)
    {
        Field* fields = rangeResult->Fetch();
        minId = fields[0].Get<uint32>();
        maxId = fields[1].Get<uint32>();
    }

    if (maxId == 0)
    {
        logInfo("No live auctions in house [" + std::to_string(auctionHouseId) + "], moving on.");
        return;
    }

    std::mt19937 engine(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint32 const span = maxId - minId + 1;
    uint32 const startId = minId + (span > 1 ? engine() % span : 0);

    std::string query = R"(
        SELECT ah.id
        FROM auctionhouse ah
        WHERE itemowner <> {} AND houseid = {} AND `time` > UNIX_TIMESTAMP() AND ah.id >= {}
        ORDER BY ah.id
        LIMIT {};
    )";

    QueryResult result = CharacterDatabase.Query(query, ownerToSkip, auctionHouseId, startId, maxQueryCount);

    std::vector<uint32> biddableAuctionIds;
    if (result)
    {
        do {
            biddableAuctionIds.push_back(result->Fetch()->Get<uint32>());
        } while(result->NextRow());
    }

    // Wrap around for the part of the window that ran past the highest id, so the sample is
    // not biased towards the end of the id range.
    if (biddableAuctionIds.size() < maxQueryCount)
    {
        std::string wrapQuery = R"(
            SELECT ah.id
            FROM auctionhouse ah
            WHERE itemowner <> {} AND houseid = {} AND `time` > UNIX_TIMESTAMP() AND ah.id < {}
            ORDER BY ah.id
            LIMIT {};
        )";

        QueryResult wrapResult = CharacterDatabase.Query(wrapQuery, ownerToSkip, auctionHouseId, startId, maxQueryCount - (uint32)biddableAuctionIds.size());
        if (wrapResult)
        {
            do {
                biddableAuctionIds.push_back(wrapResult->Fetch()->Get<uint32>());
            } while(wrapResult->NextRow());
        }
    }

    if (biddableAuctionIds.empty())
    {
        logInfo("Can't see player auctions at ["
            + std::to_string(auctionHouseId) + "] not from ["
            + std::to_string(auctionatorPlayerGuid) + "], moving on.");
        return;
    }

    std::shuffle(biddableAuctionIds.begin(), biddableAuctionIds.end(), engine);

    logInfo("Found " + std::to_string(biddableAuctionIds.size()) + " biddable auctions");

    // Price data for everything we may look at, in one query.
    std::vector<uint32> candidateEntries;
    candidateEntries.reserve(biddableAuctionIds.size());
    for (uint32 auctionId : biddableAuctionIds)
    {
        if (AuctionEntry* entry = ahMgr->GetAuction(auctionId))
        {
            candidateEntries.push_back(entry->item_template);
        }
    }
    LoadMarketData(candidateEntries);

    uint32 purchasePerCycle = purchasesPerCycle;
    uint32 counter = 0;
    uint32 const total = static_cast<uint32>(biddableAuctionIds.size());
    std::size_t nextAuction = 0;

    while(purchasePerCycle > 0 && nextAuction < biddableAuctionIds.size()) {
        counter++;
        AuctionEntry* auction = GetAuctionForPurchase(biddableAuctionIds, nextAuction);

        // The id is gone from the in-memory house (it expired or was bought out between the
        // query and this loop): skip this one instead of abandoning the whole cycle - the
        // cursor has already advanced, so this cannot loop.
        if (auction == nullptr) {
            continue;
        }

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!itemTemplate) {
            logError("Skipping auction " + std::to_string(auction->Id) + ": item template " + std::to_string(auction->item_template) + " is missing.");
            continue;
        }

        logInfo("Considering auction: "
            + itemTemplate->Name1
            + " [AuctionId: " + std::to_string(auction->Id) + "]"
            + " [ItemId: " + std::to_string(itemTemplate->ItemId) + "]"
            + " <> " + std::to_string(counter) + " of "
            + std::to_string(total)
        );

        // If this item has a buyout price, let's try to buy it. Otherwise we will
        // see if it's worth bidding on.
        bool success = false;
        if (auction->buyout > 0) {
            success = BuyoutAuction(auction, itemTemplate);
        } else {
            success = BidOnAuction(auction, itemTemplate);
        }

        if (success) {
            purchasePerCycle--;
            logInfo("Purchase made, remaining: " + std::to_string(purchasePerCycle));
        }
    }
}

AuctionEntry* AuctionatorBidder::GetAuctionForPurchase(std::vector<uint32> const& biddableAuctionIds,
    std::size_t& nextIndex)
{
    if (!ahMgr || nextIndex >= biddableAuctionIds.size()) {
        return nullptr;
    }

    //
    // A cursor, not erase(begin()): erasing from the front of the sampled window on every
    // step is O(n) per step (up to 100k ids in the window at the configured maximum), so
    // the whole walk was quadratic. The window is read-only, so an index is enough.
    //
    uint32 const auctionId = biddableAuctionIds[nextIndex++];

    logTrace("Auction consumed, remaining in window: " + std::to_string(biddableAuctionIds.size() - nextIndex));

    return ahMgr->GetAuction(auctionId);
}

static bool IsValidAuctionLifecycleState(AuctionEntry const* auction)
{
    if (!auction) {
        return false;
    }

    if (auction->Id == 0 || auction->item_guid.IsEmpty()) {
        return false;
    }

    if (auction->houseId != AuctionHouseId::Alliance &&
        auction->houseId != AuctionHouseId::Horde &&
        auction->houseId != AuctionHouseId::Neutral) {
        return false;
    }

    return true;
}

void AuctionatorBidder::LogUnfundedPurchase(char const* action, AuctionEntry const* auction, uint32 amount)
{
    if (!auction)
    {
        return;
    }

    //
    // The module has no funds of its own: nothing is escrowed for this purchase, while
    // the core still pays the seller (SendAuctionSuccessfulMail() pays "bid + deposit -
    // cut"). Every purchase is therefore newly created gold, and the item the bot wins is
    // sunk by the mail recycling. This is logged rather than prevented: it is the
    // documented behaviour of the bidder.
    //
    logWarn(std::string("UNFUNDED ") + (action ? action : "purchase")
        + " on auction " + std::to_string(auction->Id)
        + " (item " + std::to_string(auction->item_template)
        + ", owner " + std::to_string(auction->owner.GetCounter()) + "): "
        + std::to_string(amount) + " copper is not escrowed; the seller is paid with newly created gold.");
}

bool AuctionatorBidder::BidOnAuction(AuctionEntry* auction, ItemTemplate const* itemTemplate)
{
    if (!auction || !itemTemplate) {
        logError("BidOnAuction called with invalid auction state.");
        return false;
    }

    if (!IsValidAuctionLifecycleState(auction)) {
        logError("BidOnAuction skipped: auction does not satisfy the official lifecycle guard.");
        return false;
    }

    uint32 currentPrice;

    // When somebody already bid on this auction we skip it: we are not in the market of
    // outbidding players, and there is no reason to bid against ourselves either.
    if (auction->bid) {
        if (auction->bidder == buyerGuid) {
            logInfo("Skipping auction, I have already bid: "
                + std::to_string(auction->bid) + ".");
        } else {
            logInfo("Skipping auction, someone else has already bid "
                + std::to_string(auction->bid) + ".");
        }
        return false;
    } else {
        // nobody has bidded on this auction, so let's grab it's current bid value
        // for further scrutiny.
        currentPrice = auction->startbid;
    }

    // find out what we should really consider paying for the auction
    uint32 buyPrice = CalculateBuyPrice(auction, itemTemplate);

    // decide if our bid is less than the max amount we want to pay to avoid overpaying
    // for an item.
    if (currentPrice > buyPrice) {
        logInfo("Skipping auction ("
            + std::to_string(auction->Id) + "), price of "
            + std::to_string(currentPrice) + " is higher than template price of ("
            + std::to_string(buyPrice) + ")"
        );
        return false;
    }

    // We add half the difference between the current bid and our maximum to the amount we
    // bid, to help the seller out a little bit without overpaying too much. The result is
    // clamped to MAX_MONEY_AMOUNT: the core moves mail money around as int32.
    uint32 const rawBid = currentPrice + (buyPrice - currentPrice) / 2;
    uint32 const bidPrice = std::clamp<uint32>(rawBid, 1, MAX_MONEY_AMOUNT);

    auction->bidder = buyerGuid;
    auction->bid = bidPrice;

    LogUnfundedPurchase("bid", auction, bidPrice);

    // Same write the core's HandleAuctionPlaceBid does: a prepared statement, plus the
    // search cache update, so the client side "all auctions" listing does not keep
    // showing the pre-bid price.
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AUCTION_BID);
    stmt->SetData(0, auction->bidder.GetCounter());
    stmt->SetData(1, auction->bid);
    stmt->SetData(2, auction->Id);
    CharacterDatabase.Execute(stmt);

    sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);

    logInfo("Bid on auction of "
        + itemTemplate->Name1 + " ["
        + std::to_string(auction->Id) + "] of "
        + std::to_string(bidPrice) + " copper."
    );

    return true;
}

bool AuctionatorBidder::BuyoutAuction(AuctionEntry* auction, ItemTemplate const* itemTemplate)
{
    if (!auction || !itemTemplate || !sAuctionMgr || !ahMgr) {
        logError("BuyoutAuction called with invalid auction state.");
        return false;
    }

    if (!IsValidAuctionLifecycleState(auction)) {
        logError("BuyoutAuction skipped: auction does not satisfy the official lifecycle guard.");
        return false;
    }

    // let's just go ahead and find out what the max we will pay for this item is.
    uint32 buyPrice = CalculateBuyPrice(auction, itemTemplate);

    if (auction->buyout > buyPrice) {
        logInfo("Skipping buyout, price ("
            + std::to_string(auction->buyout) +") is higher than template buyprice ("
            + std::to_string(buyPrice) +")");
        return false;
    }

    ObjectGuid const previousBidder = auction->bidder;
    uint32 const previousBid = auction->bid;

    //
    // One transaction for every database write below.
    //
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    //
    // Everything that can throw (string building, MailDraft, the script hooks) runs in this
    // block, while the auction and its item are still fully intact, so a failure here can
    // restore bid/bidder and leave the world consistent.
    //
    // The teardown is deliberately NOT inside it: AuctionHouseMgr::RemoveAItem(guid, true,
    // trans) ends in Item::SaveToDB(), whose ITEM_REMOVED branch is "delete this" - after it
    // has run the item row is queued for deletion and the Item object no longer exists, so
    // there is nothing left to roll back. A catch that pretended otherwise (the previous
    // FSetState(ITEM_UNCHANGED)) could only mislead the reader.
    //
    try {
        // A player who bid on this auction already paid: the core escrows the bid
        // money on HandleAuctionPlaceBid and only returns it through
        // SendAuctionOutbiddedMail(). It must run *before* bid/bidder are overwritten,
        // because the refund mail is built from auction->bid. newBidder is intentionally
        // passed as nullptr (we have no online player object for the bot); the money mail
        // is still sent, only the in-game bidder notification is skipped.
        if (previousBidder && previousBidder != buyerGuid) {
            logInfo("Refunding the previous bidder of auction "
                + std::to_string(auction->Id) + " with " + std::to_string(previousBid) + " copper.");
            sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, nullptr, trans);
        }

        // Keep the auction record in a valid state before the official mail/cleanup flow runs.
        auction->bidder = buyerGuid;
        auction->bid = auction->buyout;

        LogUnfundedPurchase("buyout", auction, auction->buyout);

        //
        // Follow the core's buyout sequence (HandleAuctionBuyout): sale pending note, then
        // the sale mail, then the scripts' "auction successful" hook, then the teardown.
        // SendAuctionWonMail() is deliberately NOT called - the item the bot wins is sunk
        // instead of being mailed to it (its auction mail is recycled), which is the
        // documented behaviour of the module.
        //
        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sScriptMgr->OnAuctionSuccessful(ahMgr, auction);
    } catch (const std::exception& e) {
        // Nothing has been queued for deletion and the item is untouched, so the auction
        // stays alive exactly as it was.
        auction->bidder = previousBidder;
        auction->bid = previousBid;
        logError("BuyoutAuction failed before the teardown: " + std::string(e.what()));
        return false;
    }

    // Read what the final log needs before the auction entry is deleted below.
    uint32 const purchasedId = auction->Id;
    uint32 const purchasedCount = auction->itemCount;
    uint32 const purchasedPrice = auction->buyout;

    auction->DeleteFromDB(trans);
    // Deletes the item row and the Item object (see the comment on the try block above).
    sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);

    //
    // CommitTransaction() only enqueues the statements on the database thread
    // (DatabaseWorkerPool::CommitTransaction -> Enqueue), it never throws, so there is no
    // error to handle here - a failing statement is logged by the database layer.
    //
    CharacterDatabase.CommitTransaction(trans);

    // Non-throwing teardown: drops the entry from the house map and deletes it.
    ahMgr->RemoveAuction(auction);

    logInfo("Purchased auction of "
        + itemTemplate->Name1 + " ["
        + std::to_string(purchasedId) + "]"
        + "x" + std::to_string(purchasedCount) + " for "
        + std::to_string(purchasedPrice) + " copper.");

    return true;
}

uint32 AuctionatorBidder::GetAuctionsPerCycle() const
{
    if (!config) {
        return 0;
    }

    switch(auctionHouseId) {
        case (uint32)AuctionHouseId::Alliance:
            return config->allianceBidder.maxPerCycle;
        case (uint32)AuctionHouseId::Horde:
            return config->hordeBidder.maxPerCycle;
        case (uint32)AuctionHouseId::Neutral:
            return config->neutralBidder.maxPerCycle;
        default:
            return 0;
    }
}

void AuctionatorBidder::LoadMarketData(std::vector<uint32>& itemEntries)
{
    marketData.clear();

    if (itemEntries.empty() || !config) {
        return;
    }

    // De-duplicate: many auctions share an item entry, and the IN list should stay small.
    std::sort(itemEntries.begin(), itemEntries.end());
    itemEntries.erase(std::unique(itemEntries.begin(), itemEntries.end()), itemEntries.end());

    // A pool this large means MaxPerCycle is misconfigured; the auctions beyond the cap
    // simply fall back to vendor pricing rather than building a huge IN list.
    size_t const maxEntries = 1000;
    if (itemEntries.size() > maxEntries) {
        logWarn("bidder market preload truncated from " + std::to_string(itemEntries.size())
            + " to " + std::to_string(maxEntries) + " item entries (check *Bidder.MaxPerCycle).");
        itemEntries.resize(maxEntries);
    }

    std::string inList;
    for (size_t i = 0; i < itemEntries.size(); ++i) {
        if (i) {
            inList += ',';
        }
        inList += std::to_string(itemEntries[i]);
    }

    // Newest scan per entry, selected by sorting descending on the primary key
    // (entry, scan_datetime) and keeping the first row of each entry. The age comes from
    // the database's own clock; comparing a stored DATETIME to a C++ epoch would mix time
    // zones.
    std::string marketQuery = R"(
        SELECT entry, average_price, COALESCE(GREATEST(0, TIMESTAMPDIFF(SECOND, scan_datetime, NOW())), 0)
        FROM mod_auctionator_market_price
        WHERE entry IN ({})
        ORDER BY entry, scan_datetime DESC
    )";

    QueryResult result = CharacterDatabase.Query(marketQuery, inList);
    if (!result) {
        logDebug("no market data rows for the " + std::to_string(itemEntries.size()) + " candidate item(s)");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 const entry = fields[0].Get<uint32>();

        if (marketData.find(entry) == marketData.end())
        {
            MarketScan scan;
            scan.averagePrice = fields[1].Get<uint32>();
            scan.ageSeconds = fields[2].Get<uint32>();
            marketData[entry] = scan;
        }
    } while (result->NextRow());

    logDebug("market data preloaded for " + std::to_string(marketData.size())
        + " of " + std::to_string(itemEntries.size()) + " candidate item(s)");
}

uint32 AuctionatorBidder::CalculateBuyPrice(AuctionEntry const* auction, ItemTemplate const* item) const
{
    if (!config || !auction || !item) {
        return 0;
    }

    // Market price for this item, taken from the per cycle preload (LoadMarketData) so
    // this function stays free of database access. Stale imports are ignored.
    uint32 marketPrice = 0;
    auto scanIt = marketData.find(item->ItemId);
    if (scanIt != marketData.end()) {
        marketPrice = scanIt->second.averagePrice;

        uint32 const ageSeconds = scanIt->second.ageSeconds;
        uint32 const maxAgeDays = config->marketDataMaxAgeDays;

        if (marketPrice > 0 && maxAgeDays > 0 && ageSeconds > 0
            && static_cast<uint64>(ageSeconds) > static_cast<uint64>(maxAgeDays) * 24 * 60 * 60) {
            // Stale data would make the bot overpay: fall back to the vendor price cap.
            logInfo("Ignoring stale market price for [" + item->Name1 + "]: scan is older than "
                + std::to_string(maxAgeDays) + " days.");
            marketPrice = 0;
        }
    }

    // get the stack size of the item.
    uint32 stackSize = 1;
    if (item->GetMaxStackSize() > 1 && auction->itemCount > 1) {
        stackSize = auction->itemCount;
    }

    // get our multiplier configuration so we can get the right quality multiplier.
    AuctionatorPriceMultiplierConfig multiplierConfig = config->bidderMultipliers;
    uint32 quality  = item->Quality;
    float qualityMultiplier = Auctionator::GetQualityMultiplier(multiplierConfig, quality);

    // figure out if we are using our itemtemplate->BuyPrice or market price.
    uint32 price = item->BuyPrice;
    if (marketPrice > 0) {
        logInfo("Using Market over Template for bid eval [" + item->Name1 + "] " +
            std::to_string(marketPrice) + " <--> " + std::to_string(price) +
            " with multiplier of " + std::to_string(qualityMultiplier) + "x");
        price = marketPrice;
    }

    // Maximum we are willing to pay for this stack. The cap is MAX_MONEY_AMOUNT and not
    // the uint32 maximum: the bid is mailed to the seller as an int32 amount.
    //
    // There is deliberately no hidden floor of 1.0 on the multiplier: it is a configuration
    // value documented as a decimal, so a value below 1 lowers the willingness to pay (the
    // "at least 1 copper" clamp at the end of this function is the only hard limit).
    const long double multiplier = std::max<long double>(0.0L, static_cast<long double>(qualityMultiplier));
    const uint64 computed = static_cast<uint64>(std::llround(static_cast<long double>(stackSize) * static_cast<long double>(price) * multiplier));
    return static_cast<uint32>(std::min<uint64>(std::max<uint64>(1ULL, computed), MAX_MONEY_AMOUNT));
}
