
#include "AuctionatorSales.h"
#include "Auctionator.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"

#include <map>
#include <string>
#include <unordered_set>

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
    //
    // Ids of the live auctions this module created: the in-memory side of
    // mod_auctionator_auction, loaded once at startup and kept in step by RememberCreated() and
    // Forget(). The sale hook asks this set, so a sale costs no blocking database round trip
    // (it used to run one SELECT per sale - player-to-player sales included - on the world
    // thread).
    //
    // Only the world thread creates, sells and removes auctions, so this needs no locking.
    //
    std::unordered_set<uint32> LiveModuleAuctions;
    bool RegistryLoaded = false;

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

void AuctionatorSales::LoadRegistry()
{
    if (RegistryLoaded || !TableIsReady(RegistryTableName, RegistryUpdateFile))
    {
        return;
    }

    RegistryLoaded = true;

    //
    // An auction id is NOT unique over the life of a realm: the core restarts its counter at
    // MAX(auctionhouse.id) + 1 on every startup (ObjectMgr::SetHighestGuids), so once the
    // newest auctions have sold or expired their ids are handed out again. A registry row that
    // outlived its auction would then (a) collide with the row of the module's next listing,
    // and (b) make a player's auction that inherited the id look like a module listing.
    //
    // So the registry only ever describes live auctions. Rows are dropped when their auction
    // is removed (Forget()), and whatever a crash or an older build left behind is dropped
    // here, before any id can be reused: the world is still starting, nobody can list yet.
    // The owner is compared as well, which also catches an id that was already reused by a
    // player while an older build was running.
    //
    CharacterDatabase.DirectExecute(
        "DELETE r FROM " + RegistryTableName + " r "
        "LEFT JOIN auctionhouse ah ON ah.id = r.auction_id AND ah.itemowner = r.owner_guid "
        "WHERE ah.id IS NULL");

    LiveModuleAuctions.clear();

    if (QueryResult result = CharacterDatabase.Query("SELECT auction_id FROM " + RegistryTableName))
    {
        do
        {
            LiveModuleAuctions.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    logInfo("registry loaded: " + std::to_string(LiveModuleAuctions.size()) + " live module auction(s)");
}

void AuctionatorSales::RememberCreated(AuctionEntry const* auction, CharacterDatabaseTransaction trans)
{
    if (!auction || !trans || !TableIsReady(RegistryTableName, RegistryUpdateFile))
    {
        return;
    }

    LoadRegistry();
    LiveModuleAuctions.insert(auction->Id);

    // REPLACE, not INSERT: the caller's transaction also carries the item and auction rows of
    // a whole seller run, and a duplicate key here would roll all of them back while the
    // auctions stay alive in memory. LoadRegistry() and Forget() keep stale ids out of the
    // table, so this is only the last line of defence.
    trans->Append("REPLACE INTO " + RegistryTableName + " (auction_id, owner_guid, item_entry) VALUES ("
        + std::to_string(auction->Id) + ", "
        + std::to_string(auction->owner.GetCounter()) + ", "
        + std::to_string(auction->item_template) + ")");
}

void AuctionatorSales::Forget(uint32 auctionId)
{
    // Called for every auction the core removes, so the common case (a player auction, or a
    // realm that does not use the module at all) has to stay this one hash lookup.
    if (LiveModuleAuctions.erase(auctionId) == 0)
    {
        return;
    }

    CharacterDatabase.Execute("DELETE FROM " + RegistryTableName + " WHERE auction_id = {}", auctionId);
}

bool AuctionatorSales::IsRegistered(uint32 auctionId)
{
    LoadRegistry();
    return LiveModuleAuctions.find(auctionId) != LiveModuleAuctions.end();
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
