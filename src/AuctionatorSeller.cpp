#include "Auctionator.h"
#include "AuctionHouseMgr.h"
#include "AuctionatorSeller.h"
#include "Item.h"
// MAX_MONEY_AMOUNT is a macro in Player.h and is declared nowhere else, so this include is
// needed even though this file never touches a Player object.
#include "Player.h"
// sObjectMgr / sSpellMgr resolve the crafted item of a recipe for the item level gate below.
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Random.h"
#include "Timer.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    //
    // Shared candidate pool. The pool (item_template joined with the module tables) does
    // not depend on the auction house, but each of the three house events constructs its
    // own AuctionatorSeller: without this cache every house on a one minute cycle ran the
    // same heavy query. The time to live is one tick (the events are driven by the auction
    // house manager's minute timer).
    //
    // Only the world thread runs the events and the GM commands, so this needs no locking.
    //
    std::vector<CachedItem> PoolCache;
    uint32 PoolCacheMs = 0;
    uint32 const PoolCacheTtlMs = 60 * 1000;

    // Seconds in a day, used for the market data age check.
    constexpr uint64 SecondsPerDay = 24 * 60 * 60;

    // Quote a schema name so it can be used as a table qualifier.
    //
    // The names are never baked into the module: they are read at runtime from
    // worldserver.conf (WorldDatabaseInfo / CharacterDatabaseInfo, fifth ';'
    // separated field) through WorldDatabase/CharacterDatabase::GetConnectionInfo().
    // They are operator input, so only names that cannot be quoted safely are refused.
    bool QuoteDatabaseIdentifier(std::string const& name, std::string& quoted)
    {
        if (name.empty() || name.size() > 64
            || name.find_first_of("`\\") != std::string::npos)
        {
            return false;
        }

        quoted = '`' + name + '`';
        return true;
    }

    // Is the imported market price of this item usable (present and fresh enough)?
    // marketAgeSeconds == 0 means the import carried no timestamp, in which case the row
    // is trusted.
    bool HasUsableMarketPrice(CachedItem const& item, uint32 maxAgeDays)
    {
        if (item.marketPrice == 0)
        {
            return false;
        }

        if (maxAgeDays == 0 || item.marketAgeSeconds == 0)
        {
            return true;
        }

        return static_cast<uint64>(item.marketAgeSeconds) <= static_cast<uint64>(maxAgeDays) * SecondsPerDay;
    }

    // Age of the market scan in whole days.
    uint32 MarketDataAgeDays(CachedItem const& item)
    {
        return static_cast<uint32>(item.marketAgeSeconds / SecondsPerDay);
    }

    // Keep auction prices inside the range the client and AuctionHouseMgr expect.
    uint32 ClampPrice(long long value)
    {
        if (value < 1)
        {
            return 1;
        }

        return static_cast<uint32>(std::min<long long>(value, MAX_MONEY_AMOUNT));
    }

    // Level of the item a recipe teaches to craft; 0 when no such item can be resolved.
    // The craft spell is the one whose spelltrigger is ITEM_SPELLTRIGGER_LEARN_SPELL_ID; it
    // creates the product through SPELL_EFFECT_CREATE_ITEM, and the slot is not fixed, so all
    // three are checked. Enchanting formulas create no item and answer 0.
    uint32 CraftedItemLevel(ItemTemplate const* recipe)
    {
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            if (recipe->Spells[slot].SpellTrigger != ITEM_SPELLTRIGGER_LEARN_SPELL_ID
                || recipe->Spells[slot].SpellId <= 0)
            {
                continue;
            }

            SpellInfo const* spell = sSpellMgr->GetSpellInfo(static_cast<uint32>(recipe->Spells[slot].SpellId));
            if (!spell)
            {
                continue;
            }

            for (uint8 effect = 0; effect < MAX_SPELL_EFFECTS; ++effect)
            {
                SpellEffectInfo const& info = spell->GetEffect(static_cast<SpellEffIndex>(effect));
                if (info.Effect != SPELL_EFFECT_CREATE_ITEM || info.ItemType == 0)
                {
                    continue;
                }

                if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(info.ItemType))
                {
                    return product->ItemLevel;
                }
            }
        }

        return 0;
    }
}

AuctionatorSeller::AuctionatorSeller(Auctionator* natorParam)
{
    SetLogPrefix("[AuctionatorSeller] ");
    nator = natorParam;
}

void AuctionatorSeller::LetsGetToIt(uint32 maxCount, uint32 houseId)
{
    if (!nator || !nator->config) {
        logError("AuctionatorSeller::LetsGetToIt called without a valid Auctionator instance.");
        return;
    }

    if (maxCount == 0) {
        return;
    }

    //
    // On a realm that shares one auction house between the factions (the core then stores
    // every auction with houseId = Neutral and only the neutral search partition is ever
    // queried by clients), listing into Alliance or Horde would create auctions that
    // nobody can see while still occupying the house quota. CreateAuction() refuses them
    // as well; stopping here keeps the log to one clear line per cycle instead of one per
    // attempted item, and avoids the (expensive) candidate query altogether.
    //
    if (houseId != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
    {
        logError("seller skipped for house " + std::to_string(houseId)
            + ": AllowTwoSide.Interaction.Auction is enabled, so only house 7 (neutral) is visible to players. "
              "Enable Auctionator.NeutralSeller and leave the alliance/horde sellers disabled.");
        return;
    }

    // Schema names come from worldserver.conf, never from the module: the pools
    // carry the database parsed out of WorldDatabaseInfo / CharacterDatabaseInfo.
    std::string characterDbName;
    std::string worldDbName;
    if (!QuoteDatabaseIdentifier(CharacterDatabase.GetConnectionInfo()->database, characterDbName)
        || !QuoteDatabaseIdentifier(WorldDatabase.GetConnectionInfo()->database, worldDbName))
    {
        logError("cannot resolve the world/characters schema names from worldserver.conf "
            "(WorldDatabaseInfo / CharacterDatabaseInfo); seller skipped for this cycle.");
        return;
    }

    //
    // The item/class configuration and the market price table live in different databases
    // (world vs characters), so every table is fully qualified with the names read from
    // worldserver.conf above, and the query runs on the *world* connection (the world
    // account needs SELECT on the characters schema).
    //
    // Scan age is computed by the database (TIMESTAMPDIFF against NOW()) so that both
    // sides of the age check use one clock.
    //
    std::string const verifiedBuildFilter = nator->config->sellerConfig.excludeUnverifiedItems
        ? " AND it.VerifiedBuild != 1"
        : "";

    // 0 = no item level limit; read once per pool build (the pool itself is cached).
    uint32 const maxItemLevel = nator->config->sellerConfig.maxItemLevel;

    std::string cacheQuery = R"(
        SELECT
            it.entry, it.name, it.BuyPrice, it.SellPrice, it.stackable, it.quality
            , COALESCE(mp.average_price, 0) as average_price
            , COALESCE(mp.item_count, 0) as item_count
            , COALESCE(GREATEST(0, TIMESTAMPDIFF(SECOND, mp.scan_datetime, NOW())), 0) as scan_age_seconds
            , aicconf.max_count
            , COALESCE(aicconf.stack_count, 0) as stack_count
            , it.ItemLevel, it.class
        FROM
            {}.mod_auctionator_itemclass_config aicconf
            INNER JOIN {}.item_template it ON
                aicconf.class = it.class
                AND aicconf.subclass = it.subclass
                AND it.bonding != 1
                AND (aicconf.bonding = 0 OR it.bonding >= aicconf.bonding)
        )" + verifiedBuildFilter + R"(
            LEFT JOIN {}.mod_auctionator_disabled_items dis ON it.entry = dis.item
            LEFT JOIN {}.mod_auctionator_quality_config qc ON qc.quality = it.quality
            LEFT JOIN (
                SELECT mp1.entry
                    , mp1.average_price
                    , mp1.`count` as item_count
                    , mp1.scan_datetime
                FROM {}.mod_auctionator_market_price mp1
                INNER JOIN (
                    SELECT entry, MAX(scan_datetime) as max_scan
                    FROM {}.mod_auctionator_market_price
                    GROUP BY entry
                ) mp2 ON mp1.entry = mp2.entry AND mp1.scan_datetime = mp2.max_scan
            ) mp ON it.entry = mp.entry
        WHERE dis.item IS NULL
            -- Quality gate: a quality with no row (or with enabled = 1) may be listed, so an
            -- empty table keeps the old behaviour and the switch is per quality, not a floor.
            AND (qc.quality IS NULL OR qc.enabled = 1)
    )";

    //
    // The pool is fetched at most once per tick and shared by all three houses. A failed
    // query is deliberately not cached, so the next house retries.
    //
    uint32 const nowMs = getMSTime();
    bool const poolIsFresh = !PoolCache.empty() && PoolCacheMs != 0
        && getMSTimeDiff(PoolCacheMs, nowMs) < PoolCacheTtlMs;

    if (poolIsFresh)
    {
        logDebug("seller pool served from cache: " + std::to_string(PoolCache.size())
            + " item(s), age " + std::to_string(getMSTimeDiff(PoolCacheMs, nowMs)) + "ms");
    }
    else
    {
        PoolCache.clear();
        PoolCacheMs = 0;

        QueryResult result = WorldDatabase.Query(cacheQuery,
            worldDbName, worldDbName, worldDbName, worldDbName, characterDbName, characterDbName);

        if (!result)
        {
            //
            // DatabaseWorkerPool::Query() returns a null result both when the query fails
            // and when it matches zero rows (the first NextRow() is part of that check), so
            // this one message has to name every likely cause - the seller would otherwise
            // look healthy while listing nothing at all.
            //
            // The query runs on the *world* connection and joins the characters schema, so
            // the world account needs SELECT there. AzerothCore's default grants do not
            // include it, and a denied SELECT is only visible in the sql log.
            //
            logWarn("seller item query produced no rows for house " + std::to_string(houseId)
                + ". Check, in this order: (1) the world DB user has SELECT on the characters schema, because the "
                  "query joins " + characterDbName + ".mod_auctionator_market_price and " + characterDbName + ".item_instance "
                  "through the world connection (a denied SELECT only shows in the sql log); (2) "
                + worldDbName + ".mod_auctionator_itemclass_config exists and has rows; (3) "
                  "item_template is populated for the configured class/subclass pairs; (4) "
                + worldDbName + ".mod_auctionator_quality_config does not have every quality disabled.");
            return;
        }

        // Counters for the item level gate, reported below (and in the "everything was
        // dropped" message, where they are the whole explanation).
        uint32 recipesWithoutProduct = 0;
        uint32 skippedByItemLevel = 0;

        do
        {
            Field* fields = result->Fetch();
            CachedItem item;
            item.entry = fields[0].Get<uint32>();
            item.name = fields[1].Get<std::string>();
            item.basePrice = fields[2].Get<uint32>();
            item.sellPrice = fields[3].Get<uint32>();
            item.stackable = fields[4].Get<uint32>();
            item.quality = fields[5].Get<uint32>();
            item.marketPrice = fields[6].Get<uint32>();
            item.marketCount = fields[7].Get<uint32>();
            item.marketAgeSeconds = fields[8].Get<uint32>();
            item.maxCount = fields[9].Get<uint32>();
            item.stackCount = fields[10].Get<uint32>();

            // Item level gate: a recipe counts as the item it makes, never as its own
            // ItemLevel (a placeholder on recipes). A recipe whose crafted item cannot be
            // resolved is never listed, at any setting of the limit; GM listings are exempt.
            item.itemLevel = fields[11].Get<uint32>();

            if (fields[12].Get<uint32>() == ITEM_CLASS_RECIPE)
            {
                ItemTemplate const* recipe = sObjectMgr->GetItemTemplate(item.entry);
                item.itemLevel = recipe ? CraftedItemLevel(recipe) : 0;

                if (item.itemLevel == 0)
                {
                    ++recipesWithoutProduct;
                    continue;
                }
            }

            if (maxItemLevel > 0 && item.itemLevel > maxItemLevel)
            {
                ++skippedByItemLevel;
                continue;
            }

            PoolCache.push_back(item);
        } while (result->NextRow());

        if (PoolCache.empty())
        {
            // The gate removed every fetched row, so the query itself is fine: reporting the
            // counters keeps this apart from the "check the world DB grants" message above.
            if (recipesWithoutProduct > 0 || skippedByItemLevel > 0)
            {
                logWarn("item level gate dropped every one of the " + std::to_string(recipesWithoutProduct + skippedByItemLevel)
                    + " usable row(s) for house " + std::to_string(houseId) + ": "
                    + std::to_string(recipesWithoutProduct) + " recipe(s) whose crafted item cannot be resolved "
                      "(never listed by the automatic seller) and "
                    + std::to_string(skippedByItemLevel) + " item(s) above Auctionator.Seller.MaxItemLevel = "
                    + std::to_string(maxItemLevel) + ". Raise that limit, or add the class/subclass you want "
                      "listed to mod_auctionator_itemclass_config.");
                return;
            }

            logWarn("seller item query returned no usable rows for house " + std::to_string(houseId) + ".");
            return;
        }

        PoolCacheMs = nowMs;
        logDebug("seller pool refreshed: " + std::to_string(PoolCache.size()) + " item(s)"
            + " (item level gate: " + std::to_string(recipesWithoutProduct) + " recipe(s) without a crafted item, "
            + std::to_string(skippedByItemLevel) + " above " + std::to_string(maxItemLevel) + ")");
    }

    std::vector<CachedItem> const& cachedItems = PoolCache;

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

    //
    // Selection: items with fresh market data are preferred, weighted by their traded
    // volume, so the bot lists what actually trades on the reference market. Items without
    // (or with stale) market data keep a baseline weight of 1.
    //
    uint32 const maxAgeDays = nator->config->marketDataMaxAgeDays;
    bool const preferMarketItems = nator->config->sellerConfig.preferMarketItems != 0;

    struct Candidate
    {
        size_t cachedIndex = 0;
        uint64 weight = 1;
        // Decided once here and reused by the pricing loop below: HasUsableMarketPrice() is
        // a pure function of (item, maxAgeDays), so calling it twice per selected item only
        // repeated the same comparison.
        bool useMarketPrice = false;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(cachedItems.size());

    uint32 skippedByQuota = 0;
    for (size_t i = 0; i < cachedItems.size(); ++i)
    {
        CachedItem const& item = cachedItems[i];
        if (currentCounts[item.entry] >= item.maxCount)
        {
            skippedByQuota++;
            continue;
        }

        bool const useMarketPrice = HasUsableMarketPrice(item, maxAgeDays);

        uint64 weight = 1;
        if (preferMarketItems && useMarketPrice)
        {
            // Volume bias, capped so one very liquid item cannot dominate every run.
            weight = 1 + std::min<uint64>(item.marketCount, 1000);
        }

        candidates.push_back({ i, weight, useMarketPrice });
    }

    logDebug("Seller selection houseId(" + std::to_string(houseId)
        + "): cached=" + std::to_string(cachedItems.size())
        + " candidates=" + std::to_string(candidates.size())
        + " skippedByQuota=" + std::to_string(skippedByQuota)
        + " preferMarketItems=" + std::to_string(preferMarketItems ? 1 : 0));

    if (candidates.empty())
    {
        logInfo("No candidate items to list for houseId(" + std::to_string(houseId) + ") this run.");
        return;
    }

    //
    // Weighted random draw without replacement (Efraimidis & Spirakis): give every
    // candidate the key -ln(U)/weight with U uniform in (0, 1] and take the smallest keys.
    // That is equivalent to drawing items one by one with a probability proportional to
    // their weight, and guarantees that no item is drawn twice in the same run.
    //
    std::vector<std::pair<double, size_t>> ranked;    // (key, index into candidates)
    ranked.reserve(candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        // rand_norm() is [0, 1), so this is (0, 1] and log() stays finite.
        double const u = 1.0 - rand_norm();
        ranked.emplace_back(-std::log(u) / static_cast<double>(candidates[i].weight), i);
    }

    size_t const pickCount = std::min<size_t>(maxCount, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + pickCount, ranked.end());

    //
    // Create the auctions for the selected items
    //
    uint32 created = 0;

    float const bidStartModifier = std::clamp(nator->config->sellerConfig.bidStartModifier, 0.0f, 1.0f);
    bool const bidOnly = nator->config->sellerConfig.bidOnly != 0;
    float const minPriceModifier = nator->config->sellerConfig.minPriceModifier;
    float const maxPriceModifier = nator->config->sellerConfig.maxPriceModifier;
    uint32 const defaultPrice = nator->config->sellerConfig.defaultPrice;

    //
    // One transaction for the whole run, so a failure - and a rollback - stays local to
    // this house's run.
    //
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    for (size_t pick = 0; pick < pickCount; ++pick)
    {
        Candidate const& candidate = candidates[ranked[pick].second];
        CachedItem const& item = cachedItems[candidate.cachedIndex];
        std::string const& itemName = item.name;

        //
        // Stack size: mod_auctionator_itemclass_config.stack_count is the stack this
        // class/subclass is listed in, still bounded by what the item can stack to. A row
        // without a value (0/NULL) falls back to the item's own max stack.
        //
        uint32 const itemMaxStack = std::max<uint32>(1, item.stackable);
        uint32 stackSize = item.stackCount > 0 ? item.stackCount : itemMaxStack;
        if (stackSize > itemMaxStack) {
            stackSize = itemMaxStack;
        }

        if (stackSize > 1 && nator->config->sellerConfig.randomizeStackSize) {
            stackSize = GetRandomNumber(1, stackSize);
        }

        //
        // Price: the per quality multiplier only ever scales the *fallback* price (vendor
        // BuyPrice, or Auctionator.Seller.DefaultPrice). An imported market price is used
        // as-is, so market driven pricing stays market driven.
        //
        float const qualityMultiplier = Auctionator::GetQualityMultiplier(nator->config->sellerMultipliers, item.quality);
        bool const useMarketPrice = candidate.useMarketPrice;

        uint32 unitPrice = useMarketPrice ? item.marketPrice : item.basePrice;
        if (unitPrice == 0) {
            unitPrice = defaultPrice;
        }

        if (!useMarketPrice && qualityMultiplier > 0.0f) {
            unitPrice = ClampPrice(std::llround(static_cast<double>(unitPrice) * static_cast<double>(qualityMultiplier)));
        }

        //
        // Price ceiling first (it is only meaningful when the price actually came from the
        // market), then the vendor floor, so the floor always holds: a market average far
        // below the vendor sell price must not make the bot list below what a vendor pays,
        // because a player could then buy the listing and vendor it at a profit.
        //
        uint32 const floorPrice = ClampPrice(std::llround(static_cast<double>(item.sellPrice) * static_cast<double>(minPriceModifier)));

        if (useMarketPrice && maxPriceModifier > 0.0f) {
            uint32 const ceilingPrice = ClampPrice(std::llround(static_cast<double>(item.marketPrice) * static_cast<double>(maxPriceModifier)));
            if (unitPrice > ceilingPrice) {
                unitPrice = ceilingPrice;
            }
        }

        if (unitPrice < floorPrice) {
            unitPrice = floorPrice;
        }

        if (useMarketPrice) {
            logDebug("Market price [" + itemName + "]: " + std::to_string(item.marketPrice)
                + " (volume " + std::to_string(item.marketCount)
                + ", age " + std::to_string(MarketDataAgeDays(item)) + "d)"
                + " template buy " + std::to_string(item.basePrice)
                + " floor " + std::to_string(floorPrice)
                + " -> unit " + std::to_string(unitPrice));
        } else if (item.marketPrice > 0) {
            logDebug("Ignoring stale market price [" + itemName + "]: age "
                + std::to_string(MarketDataAgeDays(item)) + "d > "
                + std::to_string(maxAgeDays) + "d.");
        } else {
            logDebug("No market price [" + itemName + "]: template buy "
                + std::to_string(item.basePrice) + " floor " + std::to_string(floorPrice)
                + " -> unit " + std::to_string(unitPrice));
        }

        //
        // Starting bid: between (1 - bidStartModifier) and the unit price, never 0. A start
        // bid of 0 is accepted by the core as "any bid wins", so a player could take the
        // stack for 1 copper on the next expiry.
        //
        uint32 const bidLowerBound = std::max<uint32>(1,
            static_cast<uint32>(std::max(0.0, static_cast<double>(unitPrice) * (1.0 - bidStartModifier))));
        uint32 const safeBidLower = std::min(bidLowerBound, unitPrice);
        uint32 const safeBidUpper = std::max(bidLowerBound, unitPrice);
        uint32 const bidPrice = GetRandomNumber(safeBidLower, safeBidUpper);

        AuctionatorItem newItem;
        newItem.itemId = item.entry;
        newItem.houseId = houseId;

        uint32 const stackTotal = ClampPrice(static_cast<long long>(static_cast<uint64>(unitPrice) * static_cast<uint64>(stackSize)));

        if (bidOnly)
        {
            //
            // Pure auction: no buyout at all, so the entry can only be won by bidding. The
            // start bid is the full stack price - bidStartModifier is deliberately not
            // applied, because without a buyout there is nothing to discount from and a
            // random discount would only lower the reserve price.
            //
            newItem.buyout = 0;
            newItem.bid = stackTotal;
        }
        else
        {
            uint32 const bidValue = ClampPrice(static_cast<long long>(static_cast<uint64>(bidPrice) * static_cast<uint64>(stackSize)));
            newItem.buyout = stackTotal;
            newItem.bid = std::min<uint32>(bidValue, newItem.buyout);
        }
        newItem.time = 60 * 60 * 12;
        newItem.stackSize = stackSize;

        logDebug("Adding item: " + itemName
            + " ilvl " + std::to_string(item.itemLevel)
            + " stack " + std::to_string(newItem.stackSize)
            + " bid " + std::to_string(newItem.bid)
            + " buyout " + std::to_string(newItem.buyout)
            + " to house " + std::to_string(houseId)
        );

        if (nator->CreateAuction(newItem, trans))
        {
            created++;
        }
    }

    CharacterDatabase.CommitTransaction(trans);

    logInfo("Items added houseId("
        + std::to_string(houseId)
        + ") this run: " + std::to_string(created)
        + " of " + std::to_string(pickCount) + " selected.");
}

uint32 AuctionatorSeller::GetRandomNumber(uint32 min, uint32 max)
{
    if (min > max) {
        std::swap(min, max);
    }

    return urand(min, max);
}
