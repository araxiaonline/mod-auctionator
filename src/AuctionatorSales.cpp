
#include "AuctionatorSales.h"
#include "Auctionator.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"

#include <map>
#include <string>

namespace
{
    std::string const RegistryTableName = "mod_auctionator_auction";
    std::string const SaleTableName = "mod_auctionator_sale";

    std::string const RegistryUpdateFile = "data/sql/db-characters/updates/2026_09_27_01_auction_registry.sql";
    std::string const SaleUpdateFile = "data/sql/db-characters/updates/2026_09_27_00_sale_log.sql";

    // The module only ever records a sale it can attribute to one of its own listings, so
    // this is the whole workload of the hook: one INSERT per sold module listing.
    //
    // module_listing carries the provenance the registry established at create time (1 = ours,
    // 0 = a listing owned by the configured character but not created by the module). It is
    // recorded rather than re-derived later, because the only field that used to identify a
    // module listing - deposit = 0 - also matches every player listing on a realm whose
    // Rate.Auction.Deposit is 0.
    char const* const InsertSaleSql =
        "INSERT INTO mod_auctionator_sale "
        "(auction_id, item_entry, item_count, house_id, seller_guid, seller_is_bot, module_listing, "
        "buyer_guid, price, startbid, buyout, deposit, cut, is_buyout) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})";
}

AuctionatorSales::AuctionatorSales()
{
    SetLogPrefix("[AuctionatorSales] ");
}

bool AuctionatorSales::TableIsReady(std::string const& table, std::string const& updateFile)
{
    //
    // Probed once per process and cached per table: the answer cannot change while the
    // worldserver is running (applying the SQL update is a restart anyway), and the hook that
    // calls this runs on every single sale.
    //
    static std::map<std::string, bool> cache;

    auto cached = cache.find(table);
    if (cached != cache.end())
    {
        return cached->second;
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table);

    if (!result)
    {
        logError("cannot inspect " + table + " (is the characters database reachable?)");
        return false;
    }

    bool const ready = result->Fetch()[0].Get<uint64>() > 0;
    cache[table] = ready;

    if (!ready)
    {
        logError(table + " is missing, so what the module creates and sells is NOT recorded. "
            "Apply " + updateFile + " to the characters database.");
    }

    return ready;
}

void AuctionatorSales::RememberCreated(AuctionEntry const* auction, CharacterDatabaseTransaction trans)
{
    if (!auction || !trans || !TableIsReady(RegistryTableName, RegistryUpdateFile))
    {
        return;
    }

    // Append-only, and deliberately never deleted: an auction id is not reused within a realm
    // (sObjectMgr->GenerateAuctionID() is max+1), so keeping the row after the auction ends is
    // what lets a sale recorded later still be attributed to the module.
    trans->Append("INSERT INTO " + RegistryTableName + " (auction_id, owner_guid, item_entry) VALUES ("
        + std::to_string(auction->Id) + ", "
        + std::to_string(auction->owner.GetCounter()) + ", "
        + std::to_string(auction->item_template) + ")");
}

bool AuctionatorSales::IsRegistered(uint32 auctionId)
{
    if (!TableIsReady(RegistryTableName, RegistryUpdateFile))
    {
        return false;
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM " + RegistryTableName + " WHERE auction_id = {}", auctionId);

    return result != nullptr;
}

void AuctionatorSales::RecordSale(AuctionEntry const* auction)
{
    if (!auction || !TableIsReady(SaleTableName, SaleUpdateFile))
    {
        return;
    }

    Auctionator* auctionator = Auctionator::getInstance();
    uint32 const botGuid = auctionator->config ? auctionator->config->characterGuid : 0;
    uint32 const ownerGuid = auction->owner.GetCounter();

    //
    // A sale belongs in this log when the module created the listing (the registry knows its
    // id - the owner may be the bot or an explicit 指定角色), or when the configured character
    // owns it (a human playing that character listed it by hand; the sale is still the bot/GM's).
    //
    // Nothing is inferred from `deposit`, which used to be the test here and is not a marker at
    // all: the core charges `AH_MINIMUM_DEPOSIT * Rate.Auction.Deposit`, so a realm configured
    // with Rate.Auction.Deposit = 0 gives every player listing a deposit of 0 too - which is how
    // player-to-player sales ended up in this table.
    //
    bool const createdByModule = IsRegistered(auction->Id);
    bool const botOwned = botGuid != 0 && ownerGuid == botGuid;

    if (!createdByModule && !botOwned)
    {
        return;
    }

    //
    // A buyout sets bid = buyout before settling (WorldSession::HandleAuctionPlaceBid), so
    // "price >= buyout with a buyout set" is a buyout and anything below is a winning bid.
    // A listing without a buyout can only have been won by bidding.
    //
    uint32 const isBuyout = (auction->buyout > 0 && auction->bid >= auction->buyout) ? 1 : 0;

    CharacterDatabase.Execute(InsertSaleSql,
        auction->Id,
        auction->item_template,
        auction->itemCount,
        (uint32)auction->houseId,
        ownerGuid,
        botOwned ? 1 : 0,
        createdByModule ? 1 : 0,
        auction->bidder.GetCounter(),
        auction->bid,
        auction->startbid,
        auction->buyout,
        auction->deposit,
        auction->GetAuctionCut(),
        isBuyout);

    logDebug("recorded sale of auction " + std::to_string(auction->Id)
        + ": item " + std::to_string(auction->item_template)
        + " x" + std::to_string(auction->itemCount)
        + " to " + std::to_string(auction->bidder.GetCounter())
        + " for " + std::to_string(auction->bid) + " copper"
        + (isBuyout ? " (buyout)" : " (bid)")
        + (createdByModule ? " [module listing]" : " [bot-owned]"));
}
