
#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <vector>
#include "ScriptMgr.h"
#include "Chat.h"
#include "Auctionator.h"
#include "AuctionatorConfig.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "CharacterCache.h"
#include "AuctionatorMarketData.h"
#include "Optional.h"

using namespace Acore::ChatCommands;

namespace
{
    // Limits for the GM listing commands.
    uint32 const MaxItemsPerCommand = 200;
    uint32 const DefaultListingHours = 48;    // matches AuctionatorItem's default (172800 s)
    uint32 const MinListingHours = 1;
    uint32 const MaxListingHours = 720;       // 30 days
    uint32 const MaxListingStack = 1000;
    // Upper bound for ".auctionator multiplier": the value is multiplied into item prices
    // and std::llround() is undefined for out of range results.
    float const MaxPriceMultiplier = 1000.0f;

    bool TryParseUInt32(const std::string& value, uint32& result)
    {
        try {
            size_t idx = 0;
            unsigned long long parsed = std::stoull(value, &idx, 10);
            if (idx != value.size() || parsed > std::numeric_limits<uint32>::max()) {
                return false;
            }
            result = static_cast<uint32>(parsed);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool TryParseFloat(const std::string& value, float& result)
    {
        try {
            size_t idx = 0;
            float parsed = std::stof(value, &idx);
            if (idx != value.size()) {
                return false;
            }
            result = parsed;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // How a GM listing prices its start bid and its buyout.
    enum class ListingMode
    {
        // Follow the realm-wide Auctionator.Seller.BidOnly / BidStartModifier pair: the
        // price is a buyout with a discounted start bid, or a pure-auction start bid.
        // This is what ".auctionator add" did before mode= existed and what rows of
        // mod_auctionator_gm_list migrated from that era still ask for, so an existing
        // roster keeps behaving exactly as it used to.
        Legacy,
        // One fixed price: the buyer pays the buyout. The start bid is forced to the
        // buyout so nobody can open a 1-copper bid and sit on it until expiry.
        Buyout,
        // Auction: the GM's start bid is authoritative and the buyout is optional
        // (0 = no buyout, the item sells to the highest bidder).
        Bid,
    };

    // Unit prices in copper, i.e. per single item. AddSingleListing multiplies them by
    // the (clamped) stack and caps the result at MAX_MONEY_AMOUNT.
    struct ListingPricing
    {
        ListingMode mode = ListingMode::Legacy;
        uint32 unitBid = 0;
        uint32 unitBuyout = 0;
    };

    // Splits "mode=bid" into "mode" and "bid". Empty keys and empty values are rejected
    // so "=x" and "mode=" cannot silently mean something.
    bool SplitOption(std::string const& token, std::string& key, std::string& value)
    {
        size_t const separator = token.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= token.size())
        {
            return false;
        }

        key = token.substr(0, separator);
        value = token.substr(separator + 1);
        return true;
    }

    // Validates an explicit mode/bid/buyout combination. Prices are UNIT copper.
    // Returns false after printing a GM-readable reason.
    bool ResolveExplicitPricing(std::string const& mode, bool hasBid, uint32 unitBid,
        bool hasBuyout, uint32 unitBuyout, ChatHandler* handler, ListingPricing& pricing)
    {
        std::string resolved = mode;

        if (resolved.empty())
        {
            // No mode=: infer it from whichever price was given, so "bid=500" and
            // "buyout=5000" both work on their own. bid wins when both are present,
            // because that is the more specific of the two.
            if (hasBid && unitBid > 0)
            {
                resolved = "bid";
            }
            else if (hasBuyout && unitBuyout > 0)
            {
                resolved = "buyout";
            }
        }

        if (resolved == "buyout")
        {
            if (!hasBuyout || unitBuyout == 0)
            {
                handler->SendSysMessage("[Auctionator] add: mode=buyout needs buyout=<copper per item> above 0.");
                return false;
            }

            if (hasBid && unitBid != 0)
            {
                handler->SendSysMessage("[Auctionator] add: mode=buyout takes no start bid: the start bid is the "
                    "buyout, otherwise the stack could be won with a fraction of the price. Use mode=bid instead.");
                return false;
            }

            pricing.mode = ListingMode::Buyout;
            pricing.unitBuyout = unitBuyout;
            return true;
        }

        if (resolved == "bid")
        {
            if (!hasBid || unitBid == 0)
            {
                handler->SendSysMessage("[Auctionator] add: mode=bid needs bid=<copper per item> above 0.");
                return false;
            }

            if (hasBuyout && unitBuyout != 0 && unitBuyout < unitBid)
            {
                handler->SendSysMessage("[Auctionator] add: buyout (" + std::to_string(unitBuyout)
                    + ") must not be below the start bid (" + std::to_string(unitBid) + ").");
                return false;
            }

            pricing.mode = ListingMode::Bid;
            pricing.unitBid = unitBid;
            pricing.unitBuyout = hasBuyout ? unitBuyout : 0;
            return true;
        }

        handler->SendSysMessage("[Auctionator] add: mode must be \"buyout\" (one fixed price) or \"bid\" "
            "(auction)" + std::string(mode.empty() ? ", and no bid=/buyout= price was given either." : "."));
        return false;
    }

    // Map a mod_auctionator_gm_list.mode value onto a ListingMode. Unknown or empty
    // values fall back to Legacy, which is the column's default.
    ListingMode ListingModeFromColumn(std::string const& value)
    {
        if (value == "buyout")
        {
            return ListingMode::Buyout;
        }

        if (value == "bid")
        {
            return ListingMode::Bid;
        }

        return ListingMode::Legacy;
    }

    // Split "5500, 5501,5502" into its non-empty tokens (spaces are ignored).
    std::vector<std::string> SplitList(std::string const& value, char separator)
    {
        std::vector<std::string> parts;
        std::string current;

        for (char ch : value)
        {
            if (ch == separator)
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
            }
            else if (!std::isspace(static_cast<unsigned char>(ch)))
            {
                current.push_back(ch);
            }
        }

        if (!current.empty())
        {
            parts.push_back(current);
        }

        return parts;
    }

    // Things a GM should know about before listing an item: the bot fabricates a
    // brand new item from item_template, so uniqueness and quest checks that the
    // normal player trade path performs do not apply.
    std::string DescribeItemNotes(ItemTemplate const* proto)
    {
        std::string notes;

        if (proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1 || proto->StartQuest != 0)
        {
            notes += "quest item; ";
        }

        if (proto->Bonding == BIND_WHEN_PICKED_UP)
        {
            notes += "binds on pickup; ";
        }

        if (proto->MaxCount > 0)
        {
            notes += "unique (max " + std::to_string(proto->MaxCount) + "); ";
        }

        return notes;
    }
}

class AuctionatorCommands : public CommandScript
{
    public:
        AuctionatorCommands() : CommandScript("AuctionatorCommandScript")
        {
        }

    private:
        // Handlers receive the raw parameter vector and check its size before
        // indexing it. The previous "std::vector<const char*>" marshalling handed
        // out a pointer that could be nullptr for an empty parameter list, so any
        // missing argument turned into a null dereference inside the handler.
        static bool DispatchCommand(ChatHandler* handler, Auctionator* auctionator, const std::string& command, const std::vector<std::string>& commandParams)
        {
            if (command == "add")
            {
                auctionator->logInfo("Adding new item(s) for GM");
                return CommandAdd(commandParams, handler, auctionator);
            }

            if (command == "addlist")
            {
                auctionator->logInfo("Adding GM list items");
                return CommandAddList(commandParams, handler, auctionator);
            }

            if (command == "market")
            {
                return CommandMarketStatus(commandParams, handler, auctionator);
            }

            if (command == "marketimport")
            {
                return CommandMarketImport(commandParams, handler, auctionator);
            }

            if (command == "marketscan")
            {
                return CommandMarketScan(commandParams, handler, auctionator);
            }

            if (command == "marketprune")
            {
                return CommandMarketPrune(commandParams, handler, auctionator);
            }

            if (command == "auctionspercycle")
            {
                return CommandAuctionsPerCycle(commandParams, handler, auctionator);
            }

            if (command == "bidonown")
            {
                return CommandBidOnOwn(commandParams, handler, auctionator);
            }

            // Quick buyout switch. The flag is read for every listing the seller creates, so
            // this takes effect on the next run without a restart; the panel persists it in the
            // option file as well.
            if (command == "buyout")
            {
                return CommandSetBuyout(commandParams, handler, auctionator);
            }

            // Item level limit of the automatic seller; the panel persists it in the option file.
            if (command == "maxitemlevel")
            {
                return CommandSetMaxItemLevel(commandParams, handler, auctionator);
            }

            if (command == "bidspercycle")
            {
                return CommandBidsPerCycle(commandParams, handler, auctionator);
            }

            if (command == "disable")
            {
                return CommandDisableSeller(commandParams, handler, auctionator);
            }

            if (command == "enable")
            {
                return CommandEnableSeller(commandParams, handler, auctionator);
            }

            if (command == "expireall")
            {
                return CommandExpireAll(commandParams, handler, auctionator);
            }

            // Per-listing management of the auctions the bot already has in the house. These
            // exist because the `auctionhouse` table is only a startup cache: the live state
            // is the AuctionHouseMgr's in-memory map, so a panel that edited the row directly
            // would be silently reverted, and a panel that edited the price would be ignored
            // by the searcher (players kept seeing the old price). Both go through the live
            // AuctionEntry instead.
            if (command == "delist")
            {
                return CommandDelist(commandParams, handler, auctionator);
            }

            if (command == "reprice")
            {
                return CommandReprice(commandParams, handler, auctionator);
            }

            if (command == "multiplier")
            {
                return CommandSetMultiplier(commandParams, handler, auctionator);
            }

            if (command == "status")
            {
                ShowStatus(handler, auctionator);
                return true;
            }

            // Runtime master switch. "Auctionator.Enabled" is only read at startup, and the
            // option file is per realm, so this is what lets one realm's bot be started or
            // stopped from the panel without restarting that realm's worldserver.
            if (command == "start" || command == "on")
            {
                return CommandSetEnabled(handler, auctionator, true);
            }

            if (command == "stop" || command == "off")
            {
                return CommandSetEnabled(handler, auctionator, false);
            }

            if (command == "help")
            {
                ShowHelp(handler);
                return true;
            }

            // Report unknown subcommands instead of silently doing nothing.
            handler->SendSysMessage("[Auctionator] unknown command \"" + command + "\"; see \".auctionator help\".");
            return true;
        }

        static bool HandleCommandOptionsNew(ChatHandler* handler, Optional<std::vector<std::string>> const& args)
        {
            // The parameter is optional on purpose: Acore::ChatCommands only invokes a
            // handler once it consumed every declared argument, and a plain
            // std::vector<std::string> requires at least one token. Without the
            // Optional wrapper a bare ".auctionator" never reached this function at all,
            // so the help fallback below was dead code.
            if (!args || args->empty())
            {
                ShowHelp(handler);
                return true;
            }

            std::vector<std::string> const& values = *args;
            std::string const command = values[0].empty() ? std::string("help") : values[0];

            std::vector<std::string> commandParams;
            commandParams.reserve(values.size() - 1);
            for (size_t i = 1; i < values.size(); ++i)
            {
                commandParams.push_back(values[i]);
            }

            Auctionator* auctionator = Auctionator::getInstance();
            auctionator->logDebug("Executing command: " + command);
            return DispatchCommand(handler, auctionator, command, commandParams);
        }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTableBase =
            {
                { "auctionator", HandleCommandOptionsNew, SEC_GAMEMASTER, Console::Yes }
            };

            return commandTableBase;
        }

        static bool IsKnownAuctionHouse(uint32 auctionHouseId)
        {
            return auctionHouseId == (uint32)AuctionHouseId::Alliance
                || auctionHouseId == (uint32)AuctionHouseId::Horde
                || auctionHouseId == (uint32)AuctionHouseId::Neutral;
        }

        // Human readable owner for the chat replies. ownerGuid == 0 means the
        // configured Auctionator character, whose auction mail is deleted by the
        // mail script: the sale money is removed from the economy (gold sink).
        static std::string DescribeOwner(uint32 ownerGuid)
        {
            if (ownerGuid == 0)
            {
                return "the auctionator character (gold is recycled by the system)";
            }

            std::string name = "unknown";
            sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(ownerGuid), name);
            return name + " (guid " + std::to_string(ownerGuid) + ", GOLD SINK BYPASSED)";
        }

        // Creates one listing. Returns true only when the auction really exists.
        //
        // pricing holds UNIT copper prices; the stack multiplication and the
        // MAX_MONEY_AMOUNT cap happen here so no caller has to think about them.
        static bool AddSingleListing(uint32 auctionHouseId, uint32 itemId, ListingPricing const& pricing,
            uint32 stackSize, uint32 hours, uint32 ownerGuid, ChatHandler* handler, Auctionator* auctionator,
            bool report)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
            {
                if (report)
                {
                    handler->SendSysMessage("[Auctionator] add: skipped item " + std::to_string(itemId)
                        + " (no item_template entry).");
                }
                return false;
            }

            uint32 const maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
            uint32 const finalStack = std::min<uint32>(std::max<uint32>(1, stackSize), maxStack);

            std::string const notes = DescribeItemNotes(proto);
            if (!notes.empty())
            {
                // The chat note is GM feedback for ".auctionator add"; during
                // ".auctionator addlist" it would repeat once per row and flood the chat.
                if (report)
                {
                    handler->SendSysMessage("[Auctionator] note for " + proto->Name1 + " ["
                        + std::to_string(itemId) + "]: " + notes
                        + "the bot creates a fresh item, so uniqueness and quest checks are bypassed.");
                }
                auctionator->logWarn("add: restricted item " + proto->Name1 + " ["
                    + std::to_string(itemId) + "]: " + notes);
            }

            // Total copper for a unit price. The product is capped at MAX_MONEY_AMOUNT
            // instead of wrapping around, exactly like the original single-price path.
            auto const totalOf = [finalStack](uint32 unitPrice) -> uint32
            {
                return static_cast<uint32>(std::min<uint64>(
                    static_cast<uint64>(unitPrice) * static_cast<uint64>(finalStack), MAX_MONEY_AMOUNT));
            };

            if (report)
            {
                uint64 const requested = static_cast<uint64>(std::max(pricing.unitBid, pricing.unitBuyout))
                    * static_cast<uint64>(finalStack);
                if (requested > MAX_MONEY_AMOUNT)
                {
                    handler->SendSysMessage("[Auctionator] add: " + proto->Name1 + " [" + std::to_string(itemId)
                        + "] stack total exceeds the maximum money amount, capping the listing price at "
                        + std::to_string(MAX_MONEY_AMOUNT) + " copper.");
                }
            }

            AuctionatorItem newItem;
            newItem.houseId = auctionHouseId;
            newItem.itemId = itemId;

            switch (pricing.mode)
            {
                case ListingMode::Buyout:
                    //
                    // One fixed price. The start bid equals the buyout on purpose: the core
                    // only rejects a bid below auction->startbid, so a start bid of 0 (or a
                    // discounted one) would let a player take the whole stack for a fraction
                    // of the price when the auction expires.
                    //
                    newItem.buyout = totalOf(pricing.unitBuyout);
                    newItem.bid = newItem.buyout;
                    break;

                case ListingMode::Bid:
                    // Auction: the GM's start bid is used as given, the buyout is optional.
                    newItem.bid = totalOf(pricing.unitBid);
                    newItem.buyout = pricing.unitBuyout == 0 ? 0 : totalOf(pricing.unitBuyout);
                    break;

                case ListingMode::Legacy:
                default:
                {
                    uint32 const cappedTotal = totalOf(pricing.unitBuyout);

                    if (auctionator->config->sellerConfig.bidOnly)
                    {
                        //
                        // Pure auction (Auctionator.Seller.BidOnly = 1): no buyout, so the whole
                        // price becomes the start bid. The GM still passes a *unit* price; it is
                        // multiplied by the stack exactly like the buyout would have been.
                        //
                        newItem.buyout = 0;
                        newItem.bid = cappedTotal;
                    }
                    else
                    {
                        newItem.buyout = cappedTotal;

                        //
                        // A start bid of 0 means "any bid wins" to the core: HandleAuctionPlaceBid only
                        // rejects a bid below auction->startbid, so a 0 would let a player take the stack
                        // (including a 500000 copper epic) for 1 copper when the auction expires. Use the
                        // same rule as the automatic seller: buyout * (1 - BidStartModifier), never below 1.
                        //
                        float const bidStartModifier =
                            std::clamp(auctionator->config->sellerConfig.bidStartModifier, 0.0f, 1.0f);
                        uint64 const startBid = static_cast<uint64>(std::llround(
                            static_cast<double>(newItem.buyout) * (1.0 - static_cast<double>(bidStartModifier))));
                        newItem.bid = static_cast<uint32>(
                            std::min<uint64>(std::max<uint64>(1, startBid), newItem.buyout));
                    }
                    break;
                }
            }

            newItem.stackSize = finalStack;
            newItem.time = hours * 60 * 60;
            newItem.ownerGuid = ownerGuid;

            bool const created = auctionator->CreateAuction(newItem);

            if (report && created)
            {
                handler->SendSysMessage("[Auctionator] add: listed " + proto->Name1 + " [" + std::to_string(itemId) + "]"
                    + " x" + std::to_string(finalStack)
                    + " in house " + std::to_string(auctionHouseId)
                    + (newItem.buyout != 0
                        ? " at " + std::to_string(newItem.buyout) + " copper buyout"
                        : std::string(" as a bid-only listing (no buyout)"))
                    + ", start bid " + std::to_string(newItem.bid) + " copper"
                    + " for " + std::to_string(hours) + "h"
                    + " | owner: " + DescribeOwner(ownerGuid) + ".");
            }
            else if (report)
            {
                handler->SendSysMessage("[Auctionator] add: item " + std::to_string(itemId)
                    + " was NOT listed; see the server log for the reason.");
            }

            return created;
        }

        // Resolves the optional owner argument: "bot" (default), "me" or a character guid.
        static bool ResolveOwnerArgument(std::string const& value, ChatHandler* handler, uint32& ownerGuid)
        {
            if (value == "bot")
            {
                ownerGuid = 0;
                return true;
            }

            if (value == "me")
            {
                Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
                if (!player)
                {
                    handler->SendSysMessage("[Auctionator] add: \"me\" is only available for an in-game GM.");
                    return false;
                }

                ownerGuid = player->GetGUID().GetCounter();
                return true;
            }

            if (!TryParseUInt32(value, ownerGuid))
            {
                handler->SendSysMessage("[Auctionator] add: owner must be \"bot\", \"me\" or a character guid.");
                return false;
            }

            if (ownerGuid != 0
                && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(ownerGuid)))
            {
                handler->SendSysMessage("[Auctionator] add: there is no character with guid "
                    + std::to_string(ownerGuid) + ".");
                return false;
            }

            return true;
        }

        // .auctionator add <house> <item[,item...]> <price> [stack] [hours] [owner]
        // .auctionator add <house> <item[,item...]> mode=<buyout|bid> [bid=<copper>] [buyout=<copper>]
        //                                           [stack=<n>] [hours=<n>] [owner=<bot|me|guid>]
        //
        // Two forms, never mixed inside one command:
        //
        //   positional : <price> is a UNIT price, so the listing price becomes price * stack,
        //                and Auctionator.Seller.BidOnly decides what that price means (a
        //                buyout with a BidStartModifier start bid, or a pure auction). This
        //                is the original form, kept so existing macros keep working.
        //
        //   options    : every price is named and given per unit. mode=buyout is one fixed
        //                price that the start bid is pinned to; mode=bid is an auction with
        //                an explicit start bid and an optional buyout. The options override
        //                Auctionator.Seller.BidOnly for this listing only.
        //
        // No owner (or "bot") means the sale money is recycled by the system.
        static bool CommandAdd(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() < 3)
            {
                handler->SendSysMessage("[Auctionator] add: usage <house> <item[,item...]> <price> [stack] [hours] "
                    "[owner] | <house> <item[,item...]> mode=<buyout|bid> [bid=<copper>] [buyout=<copper>] "
                    "[stack=<n>] [hours=<n>] [owner=<bot|me|guid>]");
                return true;
            }

            uint32 auctionHouseId = 0;

            if (!TryParseUInt32(params[0], auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: house must be a number (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            if (!IsKnownAuctionHouse(auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: invalid auction house "
                    + std::to_string(auctionHouseId) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            //
            // With AllowTwoSide.Interaction.Auction the three houses share one object and the
            // core stores every auction with houseId = Neutral; clients only ever search the
            // neutral partition, so an Alliance/Horde entry would be invisible to everybody.
            // CreateAuction() refuses it too - this check only replaces its log line with a
            // GM-readable reason.
            //
            if (auctionHouseId != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
            {
                handler->SendSysMessage("[Auctionator] add: AllowTwoSide.Interaction.Auction is enabled on this realm, "
                    "so all players share the neutral house; house " + std::to_string(auctionHouseId)
                    + " would create an auction nobody can see. Use house 7 (neutral).");
                return true;
            }

            if (!auctionator->GetAuctionHouse(auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: auction house "
                    + std::to_string(auctionHouseId) + " is not initialized; check Auctionator.CharacterId / Auctionator.CharacterGuid.");
                return true;
            }

            // A "key=value" token where the price would be selects the option form; a plain
            // number keeps the positional form. A price can never contain '=', so the two
            // forms can never be confused, and they are never mixed inside one command.
            bool const optionForm = params[2].find('=') != std::string::npos;

            ListingPricing pricing;
            uint32 stackSize = 1;
            uint32 hours = DefaultListingHours;
            uint32 ownerGuid = 0;

            if (optionForm)
            {
                std::string mode;
                bool hasBid = false;
                bool hasBuyout = false;
                uint32 optionBid = 0;
                uint32 optionBuyout = 0;

                for (size_t i = 2; i < params.size(); ++i)
                {
                    std::string key;
                    std::string value;
                    if (!SplitOption(params[i], key, value))
                    {
                        handler->SendSysMessage("[Auctionator] add: \"" + params[i] + "\" is not a key=value "
                            "option; the option form takes mode=, bid=, buyout=, stack=, hours= and owner=.");
                        return true;
                    }

                    if (key == "mode")
                    {
                        mode = value;
                    }
                    else if (key == "bid")
                    {
                        if (!TryParseUInt32(value, optionBid))
                        {
                            handler->SendSysMessage("[Auctionator] add: bid must be a number of copper per item.");
                            return true;
                        }
                        hasBid = true;
                    }
                    else if (key == "buyout")
                    {
                        if (!TryParseUInt32(value, optionBuyout))
                        {
                            handler->SendSysMessage("[Auctionator] add: buyout must be a number of copper per item.");
                            return true;
                        }
                        hasBuyout = true;
                    }
                    else if (key == "stack")
                    {
                        if (!TryParseUInt32(value, stackSize))
                        {
                            handler->SendSysMessage("[Auctionator] add: stack must be a number.");
                            return true;
                        }
                    }
                    else if (key == "hours")
                    {
                        if (!TryParseUInt32(value, hours))
                        {
                            handler->SendSysMessage("[Auctionator] add: hours must be a number.");
                            return true;
                        }
                    }
                    else if (key == "owner")
                    {
                        if (!ResolveOwnerArgument(value, handler, ownerGuid))
                        {
                            return true;
                        }
                    }
                    else
                    {
                        handler->SendSysMessage("[Auctionator] add: unknown option \"" + key + "\"; the option form "
                            "takes mode=, bid=, buyout=, stack=, hours= and owner=.");
                        return true;
                    }
                }

                if (hasBid && optionBid > MAX_MONEY_AMOUNT)
                {
                    handler->SendSysMessage("[Auctionator] add: bid must be at most "
                        + std::to_string(MAX_MONEY_AMOUNT) + " copper per item.");
                    return true;
                }

                if (hasBuyout && optionBuyout > MAX_MONEY_AMOUNT)
                {
                    handler->SendSysMessage("[Auctionator] add: buyout must be at most "
                        + std::to_string(MAX_MONEY_AMOUNT) + " copper per item.");
                    return true;
                }

                if (!ResolveExplicitPricing(mode, hasBid, optionBid, hasBuyout, optionBuyout, handler, pricing))
                {
                    return true;
                }
            }
            else
            {
                if (params.size() > 6)
                {
                    handler->SendSysMessage("[Auctionator] add: usage <house> <item[,item...]> <price> [stack] "
                        "[hours] [owner] | <house> <item[,item...]> mode=<buyout|bid> [bid=<copper>] "
                        "[buyout=<copper>] [stack=<n>] [hours=<n>] [owner=<bot|me|guid>]");
                    return true;
                }

                uint32 unitPrice = 0;
                if (!TryParseUInt32(params[2], unitPrice) || unitPrice == 0 || unitPrice > MAX_MONEY_AMOUNT)
                {
                    handler->SendSysMessage("[Auctionator] add: price must be between 1 and "
                        + std::to_string(MAX_MONEY_AMOUNT) + " copper per item.");
                    return true;
                }

                if (params.size() >= 4 && !TryParseUInt32(params[3], stackSize))
                {
                    handler->SendSysMessage("[Auctionator] add: stack must be a number.");
                    return true;
                }

                if (params.size() >= 5 && !TryParseUInt32(params[4], hours))
                {
                    handler->SendSysMessage("[Auctionator] add: hours must be a number.");
                    return true;
                }

                if (params.size() >= 6 && !ResolveOwnerArgument(params[5], handler, ownerGuid))
                {
                    return true;
                }

                // The positional form keeps consulting Auctionator.Seller.BidOnly.
                pricing.mode = ListingMode::Legacy;
                pricing.unitBuyout = unitPrice;
            }

            stackSize = std::clamp<uint32>(stackSize, 1, MaxListingStack);

            if (hours < MinListingHours || hours > MaxListingHours)
            {
                handler->SendSysMessage("[Auctionator] add: hours must be between "
                    + std::to_string(MinListingHours) + " and " + std::to_string(MaxListingHours) + ".");
                return true;
            }

            std::vector<std::string> const itemTokens = SplitList(params[1], ',');
            if (itemTokens.empty())
            {
                handler->SendSysMessage("[Auctionator] add: no item id given.");
                return true;
            }

            if (itemTokens.size() > MaxItemsPerCommand)
            {
                handler->SendSysMessage("[Auctionator] add: at most " + std::to_string(MaxItemsPerCommand)
                    + " items per command (" + std::to_string(itemTokens.size()) + " given).");
                return true;
            }

            std::vector<uint32> itemIds;
            itemIds.reserve(itemTokens.size());
            for (std::string const& token : itemTokens)
            {
                uint32 itemId = 0;
                if (!TryParseUInt32(token, itemId) || itemId == 0)
                {
                    handler->SendSysMessage("[Auctionator] add: \"" + token + "\" is not a valid item id; nothing was listed.");
                    return true;
                }

                itemIds.push_back(itemId);
            }

            uint32 listed = 0;
            for (uint32 itemId : itemIds)
            {
                if (AddSingleListing(auctionHouseId, itemId, pricing, stackSize, hours, ownerGuid, handler, auctionator, true))
                {
                    listed++;
                }
            }

            if (itemIds.size() > 1 || listed != itemIds.size())
            {
                handler->SendSysMessage("[Auctionator] add: listed " + std::to_string(listed)
                    + " of " + std::to_string(itemIds.size()) + " item(s).");
            }

            return true;
        }

        // .auctionator addlist [house] [owner]
        //
        // Lists every enabled row of mod_auctionator_gm_list (world database), so a
        // curated set of special items can be (re)stocked with one command.
        static bool CommandAddList(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() > 2)
            {
                handler->SendSysMessage("[Auctionator] addlist: usage [house] [owner]");
                return true;
            }

            uint32 overrideHouse = 0;
            if (!params.empty() && !TryParseUInt32(params[0], overrideHouse))
            {
                handler->SendSysMessage("[Auctionator] addlist: house must be a number (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            if (!params.empty() && !IsKnownAuctionHouse(overrideHouse))
            {
                handler->SendSysMessage("[Auctionator] addlist: invalid auction house "
                    + std::to_string(overrideHouse) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            uint32 ownerGuid = 0;
            if (params.size() >= 2 && !ResolveOwnerArgument(params[1], handler, ownerGuid))
            {
                return true;
            }

            QueryResult result = WorldDatabase.Query(R"(
                SELECT item, mode, price, bid, stack, hours, house, owner
                FROM mod_auctionator_gm_list
                WHERE enabled = 1
                ORDER BY house, item
            )");

            if (!result)
            {
                handler->SendSysMessage("[Auctionator] addlist: no enabled rows in mod_auctionator_gm_list (world database).");
                return true;
            }

            uint32 processed = 0;
            uint32 listed = 0;
            uint32 skipped = 0;

            do
            {
                Field* fields = result->Fetch();

                uint32 const itemId = fields[0].Get<uint32>();
                std::string const mode = fields[1].Get<std::string>();
                uint32 const unitBuyout = fields[2].Get<uint32>();
                uint32 const unitBid = fields[3].Get<uint32>();
                uint32 const stackSize = fields[4].Get<uint32>();
                uint32 const hours = fields[5].Get<uint32>();
                uint32 const rowHouse = fields[6].Get<uint32>();
                uint32 const rowOwner = fields[7].Get<uint32>();

                processed++;

                uint32 const house = overrideHouse != 0 ? overrideHouse : rowHouse;
                uint32 const owner = params.size() >= 2 ? ownerGuid : rowOwner;

                if (!IsKnownAuctionHouse(house))
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (invalid house " + std::to_string(house) + ").");
                    skipped++;
                    continue;
                }

                // See CommandAdd: on a shared-house realm only the neutral house is visible.
                if (house != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (house " + std::to_string(house) + ", but AllowTwoSide.Interaction.Auction is enabled: "
                          "only house 7 exists for players).");
                    skipped++;
                    continue;
                }

                if (!auctionator->GetAuctionHouse(house))
                {
                    handler->SendSysMessage("[Auctionator] addlist: auction house " + std::to_string(house)
                        + " is not initialized; check Auctionator.CharacterId / Auctionator.CharacterGuid.");
                    skipped++;
                    continue;
                }

                ListingPricing pricing;
                pricing.mode = ListingModeFromColumn(mode);

                if (pricing.mode == ListingMode::Bid)
                {
                    // 竞拍 row: the start bid decides the listing, so it is mandatory; the
                    // buyout is optional (0 = no buyout, the item goes to the top bidder).
                    if (unitBid == 0 || unitBid > MAX_MONEY_AMOUNT)
                    {
                        handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                            + " (mode=bid needs bid between 1 and " + std::to_string(MAX_MONEY_AMOUNT)
                            + " copper per item).");
                        skipped++;
                        continue;
                    }

                    if (unitBuyout > MAX_MONEY_AMOUNT || (unitBuyout != 0 && unitBuyout < unitBid))
                    {
                        handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                            + " (buyout must be 0 or at least the start bid " + std::to_string(unitBid) + ").");
                        skipped++;
                        continue;
                    }

                    pricing.unitBid = unitBid;
                    pricing.unitBuyout = unitBuyout;
                }
                else
                {
                    // 一口价 and legacy rows are both priced by the single "price" column;
                    // only the start bid rule differs, and AddSingleListing owns that.
                    if (unitBuyout == 0 || unitBuyout > MAX_MONEY_AMOUNT)
                    {
                        handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                            + " (price must be between 1 and " + std::to_string(MAX_MONEY_AMOUNT)
                            + " copper per item).");
                        skipped++;
                        continue;
                    }

                    pricing.unitBuyout = unitBuyout;
                }

                if (hours < MinListingHours || hours > MaxListingHours)
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (hours must be between " + std::to_string(MinListingHours)
                        + " and " + std::to_string(MaxListingHours) + ").");
                    skipped++;
                    continue;
                }

                if (owner != 0
                    && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(owner)))
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (there is no character with guid " + std::to_string(owner) + ").");
                    skipped++;
                    continue;
                }

                if (AddSingleListing(house, itemId, pricing, stackSize, hours, owner, handler, auctionator, false))
                {
                    listed++;
                }
                else
                {
                    skipped++;
                }
            } while (result->NextRow());

            std::string const ownerNote = params.size() >= 2
                ? "owner override: " + DescribeOwner(ownerGuid)
                : "owner from each row (0 = gold recycled by the system)";

            handler->SendSysMessage("[Auctionator] addlist: listed " + std::to_string(listed)
                + " of " + std::to_string(processed) + " row(s), skipped " + std::to_string(skipped)
                + " | " + ownerNote + ".");

            return true;
        }

        // .auctionator market
        static bool CommandMarketStatus(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params.empty())
            {
                handler->SendSysMessage("[Auctionator] market: usage (no arguments)");
                return true;
            }

            AuctionatorConfig* config = auctionator->config;

            std::string message = "[Auctionator] Market data:\n";
            message += " Config: max age " + std::to_string(config->marketDataMaxAgeDays) + " day(s)"
                + ", retention " + std::to_string(config->marketDataRetentionDays) + " day(s)\n";
            message += " Config: import file: "
                + (config->marketDataImportFile.empty() ? std::string("(disabled)") : config->marketDataImportFile) + "\n";

            if (!config->marketDataImportFile.empty())
            {
                message += " Config: every " + std::to_string(config->marketDataImportIntervalMinutes) + " minute(s)"
                    + ", source '" + config->marketDataImportSource + "'"
                    + ", max " + std::to_string(config->marketDataImportMaxRows) + " row(s) per import\n";
            }

            AuctionatorMarketData marketData;
            AuctionatorMarketData::Stats stats;

            if (!marketData.GetStats(config->marketDataMaxAgeDays, stats))
            {
                message += " Table: not readable, see the server log.";
                handler->SendSysMessage(message);
                return true;
            }

            message += " Table: " + std::to_string(stats.totalRows) + " row(s)"
                + ", " + std::to_string(stats.distinctItems) + " distinct item(s)\n";
            message += " Items with a usable scan: " + std::to_string(stats.freshItems)
                + " (age limit " + std::to_string(config->marketDataMaxAgeDays) + " day(s), 0 = no limit)\n";
            message += " Newest scan: " + stats.newestScan + " | oldest scan: " + stats.oldestScan;

            handler->SendSysMessage(message);
            return true;
        }

        // .auctionator marketimport [force]
        static bool CommandMarketImport(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            bool force = false;
            if (!params.empty())
            {
                if (params.size() > 1 || params[0] != "force")
                {
                    handler->SendSysMessage("[Auctionator] marketimport: usage [force]");
                    return true;
                }

                force = true;
            }

            std::string const file = auctionator->config->marketDataImportFile;
            if (file.empty())
            {
                handler->SendSysMessage("[Auctionator] marketimport: no Auctionator.MarketData.ImportFile configured.");
                return true;
            }

            AuctionatorMarketData marketData;
            AuctionatorMarketData::ImportResult const result = marketData.ImportFromFile(
                file,
                auctionator->config->marketDataImportSource,
                auctionator->config->marketDataImportMaxRows,
                force
            );

            if (!result.ok)
            {
                handler->SendSysMessage("[Auctionator] marketimport: nothing was imported ("
                    + std::to_string(result.read) + " line(s) read, "
                    + std::to_string(result.invalid) + " rejected); see the server log and \".auctionator market\".");
                return true;
            }

            if (result.unchanged)
            {
                handler->SendSysMessage("[Auctionator] marketimport: " + file
                    + " is unchanged since the last import (use \"marketimport force\" to import anyway).");
                return true;
            }

            handler->SendSysMessage("[Auctionator] marketimport: wrote " + std::to_string(result.imported)
                + " row(s), skipped " + std::to_string(result.invalid)
                + " line(s) of " + std::to_string(result.read)
                + (result.truncated ? " (TRUNCATED at Auctionator.MarketData.ImportMaxRows)" : "")
                + ". Check \".auctionator market\".");

            return true;
        }

        // .auctionator marketscan
        //
        // Prices this realm's own auction house into mod_auctionator_market_price, so the seller
        // gets market prices without an external CSV export. The aggregation itself is one SQL
        // statement (see AuctionatorMarketData::ScanAuctionHouse); the same routine runs on a
        // timer when Auctionator.MarketData.ScanIntervalMinutes is above 0.
        static bool CommandMarketScan(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params.empty())
            {
                handler->SendSysMessage("[Auctionator] marketscan: usage (no arguments)");
                return true;
            }

            bool const excludeSelf = auctionator->config->marketDataScanExcludeSelf != 0;

            AuctionatorMarketData marketData;
            AuctionatorMarketData::ScanResult const result = marketData.ScanAuctionHouse(
                excludeSelf ? auctionator->config->characterGuid : 0
            );

            if (!result.ok)
            {
                handler->SendSysMessage("[Auctionator] marketscan: failed, see the server log ("
                    + std::to_string(result.totalListings) + " listing(s) were readable).");
                return true;
            }

            handler->SendSysMessage("[Auctionator] marketscan: wrote " + std::to_string(result.rows)
                + " item price(s) from " + std::to_string(result.listings) + " listing(s) of "
                + std::to_string(result.totalListings) + " in the house"
                + (excludeSelf
                    ? " (excluded " + std::to_string(result.skippedSelf) + " belonging to the auctionator)"
                    : " (the auctionator's own listings are included)")
                + ". Check \".auctionator market\".");

            if (result.rows == 0 && excludeSelf && result.skippedSelf == result.totalListings)
            {
                // The honest explanation for the most likely empty result: nothing to sample but
                // the bot's own stock, which is excluded on purpose.
                handler->SendSysMessage("[Auctionator] marketscan: every listing in this house belongs to the "
                    "auctionator itself. Set Auctionator.MarketData.ScanExcludeSelf = 0 to sample those as well, "
                    "or point Auctionator.MarketData.ImportFile at an external export.");
            }

            return true;
        }

        // .auctionator marketprune [days]
        static bool CommandMarketPrune(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            uint32 days = auctionator->config->marketDataRetentionDays;

            if (!params.empty())
            {
                if (params.size() > 1 || !TryParseUInt32(params[0], days))
                {
                    handler->SendSysMessage("[Auctionator] marketprune: usage [days]");
                    return true;
                }
            }

            if (days < 1)
            {
                handler->SendSysMessage("[Auctionator] marketprune: the retention must be at least 1 day.");
                return true;
            }

            AuctionatorMarketData marketData;
            uint32 const removed = marketData.PruneOlderThan(days);

            handler->SendSysMessage("[Auctionator] marketprune: removed " + std::to_string(removed)
                + " row(s) older than " + std::to_string(days) + " day(s).");

            return true;
        }

        static void ShowHelp(ChatHandler* handler)
        {
            std::string helpString(R"(
Auctionator Help:
add <house> <item[,item...]> <price> [stack] [hours] [owner]
add <house> <item[,item...]> mode=<buyout|bid> [bid=<copper>] [buyout=<copper>]
                            [stack=<n>] [hours=<n>] [owner=<bot|me|guid>]
     house: 2 = alliance, 6 = horde, 7 = neutral
            (with AllowTwoSide.Interaction.Auction only house 7 is visible to
             players; houses 2 and 6 are refused instead of creating an
             auction nobody can see)
     Every price is per single item: the listing price is the unit price times
     the stack, capped at the maximum money amount. The two forms are never
     mixed in one command.
     Positional form: <price> is the unit price and Auctionator.Seller.BidOnly
     decides what it means:
       BidOnly = 0: buyout = price * stack, start bid = buyout *
                    (1 - Auctionator.Seller.BidStartModifier), at least 1
       BidOnly = 1: no buyout at all, "price * stack" is the start bid
     Option form (overrides Auctionator.Seller.BidOnly for this listing):
       mode=buyout buyout=<copper>: one fixed price. The start bid is pinned to
                    the buyout so nobody can win the stack with a low bid that
                    then expires.
       mode=bid    bid=<copper> [buyout=<copper>]: auction. The start bid is
                    required; the buyout is optional and may not be below it.
       mode= is inferred from bid=/buyout= when it is left out.
     stack: default 1, capped by the item's max stack
     hours: default 48, range 1..720
     owner: "bot" (default) recycles the sale money, "me" or a character guid
addlist [house] [owner]
     lists every enabled row of mod_auctionator_gm_list, honouring each row's
     mode (legacy / buyout / bid), price, bid, stack, hours and owner
market
     market price table: rows, distinct items, usable scans, import config
marketimport [force]
     import the CSV configured in Auctionator.MarketData.ImportFile now
marketscan
     price this realm's own auction house into mod_auctionator_market_price:
     one row per item, per-unit prices, buyout preferred and the start bid used
     only for items whose listings carry no buyout at all. No external CSV needed.
     Auctionator.MarketData.ScanExcludeSelf = 1 (default) leaves the auctionator's
     own listings out of the sample; set it to 0 on a realm whose house only holds
     the bot's stock. Run it on a timer with Auctionator.MarketData.ScanIntervalMinutes.
marketprune [days]
     delete market scans older than the retention (default from the config)
auctionspercycle <value>
     set how many auctions each seller run may add (shared by all houses)
bidspercycle <value>
     set how many auctions each bidder run may buy (all three houses)
bidonown <0|1>
     let the bidder bid on the auctionator's own auctions (testing only)
buyout <0|1>
     1 = listings carry a buyout (Auctionator.Seller.BidOnly = 0) and the start bid
         is derived from Auctionator.Seller.BidStartModifier
     0 = no buyout at all (Auctionator.Seller.BidOnly = 1): the computed price
         becomes the start bid and the entry can only be won by bidding
     Applies to the whole realm at once (automatic seller, the positional
     ".auctionator add" form and every gm_list row still on mode = legacy) and takes
     effect on the next seller run. Only the running configuration changes; the
     panel's buyout switch also writes Auctionator.Seller.BidOnly to the option
     file so the choice survives a restart.
maxitemlevel <value|off>
     Highest item level the AUTOMATIC seller may list; off or 0 = no limit.
     A recipe counts as the item it makes, not as its own ItemLevel, and a recipe
     whose crafted item cannot be resolved is never listed. GM listings (add /
     addlist / mod_auctionator_gm_list) are unaffected. Takes effect on the next
     seller run; the panel's item filter tab writes the option file.
disable <hordeseller|allianceseller|neutralseller|hordebidder|alliancebidder|neutralbidder|all>
enable <same targets as disable>
start | on
     runtime master switch ON (Auctionator.Enabled is only read at startup, so this is
     what starts a realm's bot without restarting its worldserver). Runtime only: write
     Auctionator.Enabled = 1 in this realm's mod_auctionator.conf to keep it after a restart.
stop | off
     runtime master switch OFF: clears the pending events. Already listed auctions stay.
expireall <house> [all]
     without "all" only the auctionator's own auctions are expired; listings
     created with an explicit owner need "all"
delist <auctionId> [all]
     take ONE auction down by id (the panel's listing table shows the id). The
     item is mailed back to the owner on the next auction house tick. Refused
     for an auction that already has a bid, and without "all" for one the
     auctionator does not own.
reprice <auctionId> <startbid> <buyout> [all]
     rewrite the two prices of ONE auction, in TOTAL copper (both are shown
     per row by the panel, unlike "add", whose prices are per item).
     buyout 0 = no buyout (pure auction); otherwise it may not be below the
     start bid. Applies to players immediately. Same "all" rule as delist.
multiplier <seller|bidder> <poor|normal|uncommon|rare|epic|legendary> <value>
status
help
            )");
            handler->SendSysMessage(helpString);
        }

        static std::string DescribeHouseAuctionCount(Auctionator* auctionator, AuctionHouseId houseId)
        {
            // CountAuctions() counts by AuctionEntry::houseId, which is the only correct
            // answer while CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION makes all houses
            // share one object.
            if (!auctionator->GetAuctionHouse((uint32)houseId))
            {
                return "n/a (auction houses not initialized)";
            }

            return std::to_string(auctionator->CountAuctions((uint32)houseId));
        }

        static void ShowStatus(ChatHandler* handler, Auctionator* auctionator)
        {
            std::string statusString = "[Auctionator] Status:\n\n";

            statusString += " Enabled (master switch, effective now): "
                + std::to_string(auctionator->config->isEnabled)
                + (auctionator->config->isEnabled
                    ? " (running)"
                    : " (stopped; \".auctionator start\" turns it on)")
                + "\n\n";
            statusString += " Bid on Own: " + std::to_string(auctionator->config->bidOnOwn) + "\n";
            statusString += " CharacterGuid: " + std::to_string(auctionator->config->characterGuid) + "\n";

            if (Auctionator::UsesSharedNeutralAuctionHouse())
            {
                statusString += " NOTE: AllowTwoSide.Interaction.Auction is enabled, so the three houses share one\n";
                statusString += "       object and only the neutral house (7) is visible to players. Houses 2 and 6\n";
                statusString += "       are refused by the add/addlist commands and their seller/bidder events do\n";
                statusString += "       nothing but log an error; use Auctionator.NeutralSeller / NeutralBidder.\n";
            }

            statusString += " Horde:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->hordeSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->hordeSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Horde) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->hordeBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->hordeBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->hordeBidder.maxPerCycle) + "\n";

            statusString += " Alliance:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->allianceSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->allianceSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Alliance) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->allianceBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->allianceBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->allianceBidder.maxPerCycle) + "\n";

            statusString += " Neutral:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->neutralSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->neutralSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Neutral) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->neutralBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->neutralBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->neutralBidder.maxPerCycle) + "\n";

            statusString += " Seller Multipliers:\n";
            statusString += "    Poor: " + std::to_string(auctionator->config->sellerMultipliers.poor) + "\n";
            statusString += "    Normal: " + std::to_string(auctionator->config->sellerMultipliers.normal) + "\n";
            statusString += "    Uncommon: " + std::to_string(auctionator->config->sellerMultipliers.uncommon) + "\n";
            statusString += "    Rare: " + std::to_string(auctionator->config->sellerMultipliers.rare) + "\n";
            statusString += "    Epic: " + std::to_string(auctionator->config->sellerMultipliers.epic) + "\n";
            statusString += "    Legendary: " + std::to_string(auctionator->config->sellerMultipliers.legendary) + "\n";

            statusString += " Bidder Multipliers:\n";
            statusString += "    Poor: " + std::to_string(auctionator->config->bidderMultipliers.poor) + "\n";
            statusString += "    Normal: " + std::to_string(auctionator->config->bidderMultipliers.normal) + "\n";
            statusString += "    Uncommon: " + std::to_string(auctionator->config->bidderMultipliers.uncommon) + "\n";
            statusString += "    Rare: " + std::to_string(auctionator->config->bidderMultipliers.rare) + "\n";
            statusString += "    Epic: " + std::to_string(auctionator->config->bidderMultipliers.epic) + "\n";
            statusString += "    Legendary: " + std::to_string(auctionator->config->bidderMultipliers.legendary) + "\n";

            statusString += " Seller settings:\n";
            statusString += "    Auctions per run: " + std::to_string(auctionator->config->sellerConfig.auctionsPerRun) + "\n";
            statusString += "    Default Price (no vendor/market price): " + std::to_string(auctionator->config->sellerConfig.defaultPrice) + "\n";
            statusString += "    Randomize Stack Size: " + std::to_string(auctionator->config->sellerConfig.randomizeStackSize) + "\n";
            statusString += "    Bid Start Modifier: " + std::to_string(auctionator->config->sellerConfig.bidStartModifier) + "\n";
            // Surfaced for the panel's quick switch: "off" means Auctionator.Seller.BidOnly = 1,
            // i.e. listings have no buyout and can only be won by bidding.
            statusString += "    Buyout mode: " + std::string(auctionator->config->sellerConfig.bidOnly != 0 ? "off (bidding only)" : "on")
                + " (Auctionator.Seller.BidOnly = " + std::to_string(auctionator->config->sellerConfig.bidOnly != 0 ? 1 : 0) + ")\n";
            statusString += "    Market data max age (days, seller+bidder, 0 = never): " + std::to_string(auctionator->config->marketDataMaxAgeDays) + "\n";
            statusString += "    Prefer market items: " + std::to_string(auctionator->config->sellerConfig.preferMarketItems) + "\n";
            statusString += "    Exclude VerifiedBuild = 1 items: " + std::to_string(auctionator->config->sellerConfig.excludeUnverifiedItems) + "\n";
            // Automatic seller only; a recipe counts as the item it makes.
            statusString += "    Max item level, automatic seller only (0 = no limit): "
                + std::to_string(auctionator->config->sellerConfig.maxItemLevel) + "\n";
            statusString += "    Min price modifier (x SellPrice): " + std::to_string(auctionator->config->sellerConfig.minPriceModifier) + "\n";
            statusString += "    Max price modifier (x market avg): " + std::to_string(auctionator->config->sellerConfig.maxPriceModifier) + "\n";

            handler->SendSysMessage(statusString);
        }

        // .auctionator expireall <house> [all]
        //
        // Without "all" only the auctionator's own auctions are expired, so a real
        // player's auction is never cancelled by this command.
        static bool CommandExpireAll(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] expireall: No Auction House Specified! [2, 6, 7]");
                auctionator->logInfo("expireall: No Auction House Specified!");
                return true;
            }

            if (params.size() > 2) {
                handler->SendSysMessage("[Auctionator] expireall: usage <house> [all]");
                return true;
            }

            uint32 houseId = 0;
            if (!TryParseUInt32(params[0], houseId)) {
                handler->SendSysMessage("[Auctionator] expireall: House id must be a number.");
                return true;
            }

            // Validate here as well: ExpireAllAuctions() only rejects an unknown house in
            // the debug log, so without this check the GM saw the "warning" and the "Done"
            // lines for a house that was never touched.
            if (!IsKnownAuctionHouse(houseId))
            {
                handler->SendSysMessage("[Auctionator] expireall: invalid auction house "
                    + std::to_string(houseId) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            bool includePlayerAuctions = false;
            if (params.size() >= 2)
            {
                if (params[1] != "all")
                {
                    handler->SendSysMessage("[Auctionator] expireall: usage <house> [all]");
                    return true;
                }

                includePlayerAuctions = true;
            }

            if (includePlayerAuctions)
            {
                handler->SendSysMessage("[Auctionator] expireall: WARNING - \"all\" also cancels player auctions in house "
                    + std::to_string(houseId) + " (their items and bids are returned by the core, nothing is destroyed).");
            }
            else
            {
                handler->SendSysMessage("[Auctionator] Expiring auctionator auctions for house: "
                    + std::to_string(houseId)
                    + " (listings created with an explicit owner via \".auctionator add ... <owner>\" are not"
                      " touched by this; pass \"all\" to include them).");
            }

            auctionator->logInfo("expireall: Expiring auctions for house: "
                + std::to_string(houseId)
                + (includePlayerAuctions ? " (including player auctions)" : " (auctionator owned only)"));

            auctionator->ExpireAllAuctions(houseId, includePlayerAuctions);

            auctionator->logInfo("expireall: Done for house: " + std::to_string(houseId));

            return true;
        }

        // .auctionator delist <auctionId> [all]
        //
        // Takes ONE auction down. Unlike expireall this is the targeted version, so it
        // refuses an auction that already carries a bid (the core's expiry path would settle
        // it with the bidder, i.e. sell it, which is the opposite of taking it down) and,
        // without "all", one the auctionator does not own.
        static bool CommandDelist(std::vector<std::string> const& params, ChatHandler* handler,
            Auctionator* auctionator)
        {
            if (params.empty() || params.size() > 2)
            {
                ReplyAcked(handler, false, "[Auctionator] delist: usage <auctionId> [all]");
                return true;
            }

            uint32 auctionId = 0;
            if (!TryParseUInt32(params[0], auctionId) || auctionId == 0)
            {
                ReplyAcked(handler, false, "[Auctionator] delist: the auction id must be a number above 0.");
                return true;
            }

            bool includePlayerAuctions = false;
            if (params.size() >= 2)
            {
                if (params[1] != "all")
                {
                    ReplyAcked(handler, false, "[Auctionator] delist: usage <auctionId> [all]");
                    return true;
                }

                includePlayerAuctions = true;
            }

            std::string error;
            if (!auctionator->DelistAuction(auctionId, includePlayerAuctions, error))
            {
                auctionator->logWarn("delist: refused for auction " + std::to_string(auctionId) + ": " + error);
                ReplyAcked(handler, false, "[Auctionator] delist: " + error);
                return true;
            }

            ReplyAcked(handler, true, "[Auctionator] delist: auction " + std::to_string(auctionId)
                + " is now cancelled. Its item is mailed back to the owner on the next auction "
                  "house tick (about a minute); the bot's own mail is recycled, so its stock "
                  "simply disappears.");

            return true;
        }

        // .auctionator reprice <auctionId> <startbid> <buyout> [all]
        //
        // Both prices are TOTALS in copper - exactly the two values the panel's listing table
        // shows for that row - unlike ".auctionator add", whose prices are per single item.
        // buyout 0 means "no buyout" (a pure auction). Both values are required on purpose: a
        // partial update would silently leave a price the GM meant to change.
        static bool CommandReprice(std::vector<std::string> const& params, ChatHandler* handler,
            Auctionator* auctionator)
        {
            if (params.size() < 3 || params.size() > 4)
            {
                ReplyAcked(handler, false, "[Auctionator] reprice: usage <auctionId> <startbid> <buyout> [all] "
                    "(both prices are totalled in copper; buyout 0 = no buyout)");
                return true;
            }

            uint32 auctionId = 0;
            uint32 startbid = 0;
            uint32 buyout = 0;

            if (!TryParseUInt32(params[0], auctionId) || auctionId == 0)
            {
                ReplyAcked(handler, false, "[Auctionator] reprice: the auction id must be a number above 0.");
                return true;
            }

            if (!TryParseUInt32(params[1], startbid))
            {
                ReplyAcked(handler, false, "[Auctionator] reprice: startbid must be a number of copper.");
                return true;
            }

            if (!TryParseUInt32(params[2], buyout))
            {
                ReplyAcked(handler, false, "[Auctionator] reprice: buyout must be a number of copper (0 = no buyout).");
                return true;
            }

            bool includePlayerAuctions = false;
            if (params.size() == 4)
            {
                if (params[3] != "all")
                {
                    ReplyAcked(handler, false, "[Auctionator] reprice: usage <auctionId> <startbid> <buyout> [all]");
                    return true;
                }

                includePlayerAuctions = true;
            }

            std::string error;
            if (!auctionator->RepriceAuction(auctionId, startbid, buyout, includePlayerAuctions, error))
            {
                auctionator->logWarn("reprice: refused for auction " + std::to_string(auctionId) + ": " + error);
                ReplyAcked(handler, false, "[Auctionator] reprice: " + error);
                return true;
            }

            ReplyAcked(handler, true, "[Auctionator] reprice: auction " + std::to_string(auctionId)
                + " now has start bid " + std::to_string(startbid) + " and buyout "
                + (buyout == 0 ? std::string("none") : std::to_string(buyout) + " copper")
                + ". Players see the new price immediately.");

            return true;
        }

        static void WarnIfModuleDisabled(ChatHandler* handler, Auctionator* auctionator)
        {
            if (!auctionator->config->isEnabled)
            {
                handler->SendSysMessage("[Auctionator] note: the module master switch is off "
                    "(Auctionator.Enabled = 0), so this flag has no effect yet. "
                    "\".auctionator start\" turns the module on right now; the AGMP panel's start button also "
                    "writes Auctionator.Enabled = 1 so it stays on after a restart.");
            }
        }

        // Console/SOAP replies of the state-changing commands carry the AGMP ack marker, so
        // the panel's strict check can tell "the command reached the game" from "the SOAP
        // call went nowhere". A player running the command in game sees the plain text.
        static void ReplyAcked(ChatHandler* handler, bool success, std::string const& message)
        {
            char const* const marker = success ? "[AGMP_OK] " : "[AGMP_ERROR] ";
            handler->SendSysMessage((handler->GetSession() == nullptr ? std::string(marker) : std::string()) + message);
        }

        // Shared by ".auctionator start" / ".auctionator stop". The flag is runtime only:
        // Auctionator.Enabled is the persistent copy and is owned by the AGMP panel (which
        // writes the realm's own option file before sending this command).
        static bool CommandSetEnabled(ChatHandler* handler, Auctionator* auctionator, bool enable)
        {
            char const* const verb = enable ? "start" : "stop";

            if (!auctionator->config)
            {
                ReplyAcked(handler, false, std::string("[Auctionator] ") + verb + ": config is not initialized yet.");
                return true;
            }

            if (auctionator->SetEnabled(enable) != enable)
            {
                ReplyAcked(handler, false, std::string("[Auctionator] ") + verb + ": could not change the master switch.");
                auctionator->logError(std::string(verb) + ": SetEnabled() refused the change.");
                return true;
            }

            auctionator->logInfo(std::string(verb) + ": master switch is now "
                + (enable ? "on" : "off") + " (runtime only)");
            ReplyAcked(handler, true, std::string("[Auctionator] ") + verb + ": the module is now "
                + (enable ? "ON" : "OFF")
                + (enable
                    ? ". Enabled events start about a minute from now; per-house flags still decide what runs."
                    : ". Pending events cleared; player auctions already listed are untouched.")
                + " This is a runtime switch only - "
                + (enable ? "set Auctionator.Enabled = 1" : "set Auctionator.Enabled = 0")
                + " in configs/modules/mod_auctionator.conf to make it survive a restart.");

            return true;
        }

        // Shared by enable/disable: returns false when the target is unknown.
        static bool ApplySellerToggle(std::string const& target, bool enable, Auctionator* auctionator)
        {
            AuctionatorConfig* config = auctionator->config;
            uint32 const value = enable ? 1 : 0;

            if (target == "hordeseller")          { config->hordeSeller.enabled = value; }
            else if (target == "allianceseller")  { config->allianceSeller.enabled = value; }
            else if (target == "neutralseller")   { config->neutralSeller.enabled = value; }
            else if (target == "hordebidder")     { config->hordeBidder.enabled = value; }
            else if (target == "alliancebidder")  { config->allianceBidder.enabled = value; }
            else if (target == "neutralbidder")   { config->neutralBidder.enabled = value; }
            else if (target == "all")
            {
                config->hordeSeller.enabled = value;
                config->allianceSeller.enabled = value;
                config->neutralSeller.enabled = value;
                config->hordeBidder.enabled = value;
                config->allianceBidder.enabled = value;
                config->neutralBidder.enabled = value;
            }
            else
            {
                return false;
            }

            // The flags are only read when the events are scheduled, so apply the
            // change to the running schedule right away (no restart needed).
            auctionator->ResyncEventSchedule();

            return true;
        }

        static bool CommandEnableSeller(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] enable: No target specified! [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                auctionator->logInfo("enable: No target specified!");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] enable: usage <target> (exactly one target).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            std::string const target = params[0];
            if (!ApplySellerToggle(target, true, auctionator))
            {
                handler->SendSysMessage("[Auctionator] enable: unknown target \"" + target
                    + "\" [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                return true;
            }

            auctionator->logInfo("enable: " + target);
            handler->SendSysMessage("[Auctionator] enable: " + target + " enabled, schedule updated.");
            return true;
        }

        static bool CommandDisableSeller(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] disable: No target specified! [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                auctionator->logInfo("disable: No target specified!");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] disable: usage <target> (exactly one target).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            std::string const target = params[0];
            if (!ApplySellerToggle(target, false, auctionator))
            {
                handler->SendSysMessage("[Auctionator] disable: unknown target \"" + target
                    + "\" [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                return true;
            }

            auctionator->logInfo("disable: " + target);
            handler->SendSysMessage("[Auctionator] disable: " + target + " disabled, schedule updated.");
            return true;
        }

        static bool CommandSetMultiplier(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() < 1) {
                handler->SendSysMessage("[Auctionator] multiplier: No type specified! [seller, bidder]");
                auctionator->logInfo("multiplier: No type specified");
                return true;
            }

            if (params.size() < 2) {
                handler->SendSysMessage("[Auctionator] multiplier: No quality specified! [poor, normal, uncommon, rare, epic, legendary]");
                auctionator->logInfo("multiplier: No quality specified");
                return true;
            }

            if (params.size() < 3) {
                handler->SendSysMessage("[Auctionator] multiplier: No multiplier specified!");
                auctionator->logInfo("multiplier: No multiplier specified!");
                return true;
            }

            if (params.size() > 3) {
                handler->SendSysMessage("[Auctionator] multiplier: usage <seller|bidder> <quality> <value>");
                return true;
            }

            std::string type(params[0]);
            std::string quality(params[1]);
            float newMultiplier = 0.0f;
            if (!TryParseFloat(params[2], newMultiplier)) {
                handler->SendSysMessage("[Auctionator] multiplier: Number must be a valid float.");
                return true;
            }

            if (!std::isfinite(newMultiplier) || newMultiplier < 0.0f) {
                handler->SendSysMessage("[Auctionator] multiplier: Value must be a finite non-negative float.");
                return true;
            }

            if (newMultiplier > MaxPriceMultiplier)
            {
                handler->SendSysMessage("[Auctionator] multiplier: capped at "
                    + std::to_string(MaxPriceMultiplier) + ".");
                newMultiplier = MaxPriceMultiplier;
            }

            AuctionatorPriceMultiplierConfig* multipliers;

            if (type == "seller") {
                multipliers = &auctionator->config->sellerMultipliers;
            } else if (type == "bidder") {
                multipliers = &auctionator->config->bidderMultipliers;
            } else {
                handler->SendSysMessage("[Auctionator] multiplier: Invalid type! [seller, bidder]");
                return true;
            }

            bool success = false;

            if (quality == "poor") {
                multipliers->poor = newMultiplier;
                success = true;
            } else if (quality == "normal") {
                multipliers->normal = newMultiplier;
                success = true;
            } else if (quality == "uncommon") {
                multipliers->uncommon = newMultiplier;
                success = true;
            } else if (quality == "rare") {
                multipliers->rare = newMultiplier;
                success = true;
            } else if (quality == "epic") {
                multipliers->epic = newMultiplier;
                success = true;
            } else if (quality == "legendary") {
                multipliers->legendary = newMultiplier;
                success = true;
            }

            if (success) {
                handler->SendSysMessage("[Auctionator] " + type +
                    " multiplier: " + quality + " quality multiplier set to "
                    + std::to_string(newMultiplier));
            } else {
                handler->SendSysMessage("[Auctionator] unable to set multiplier");
            }

            return true;
        }

        // .auctionator buyout <0|1>
        //
        // 1 = listings carry a buyout again (Auctionator.Seller.BidOnly = 0), so the computed
        // price is the buyout and the start bid is derived from BidStartModifier as usual.
        // 0 = no buyout at all (Auctionator.Seller.BidOnly = 1): the computed price becomes the
        // start bid and the entry can only be won by bidding.
        //
        // Only the running configuration is changed, never the option file: this is the "apply
        // it now" half of the panel's buyout switch, which persists the same choice into
        // mod_auctionator.conf itself. The seller reads the flag for every listing it creates,
        // so the next run already follows it - no restart involved.
        static bool CommandSetBuyout(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() != 1)
            {
                handler->SendSysMessage("[Auctionator] buyout: usage <0|1> "
                    "(1 = listings have a buyout, 0 = no buyout, bidding only)");
                return true;
            }

            uint32 enabled = 0;
            if (!TryParseUInt32(params[0], enabled) || enabled > 1)
            {
                handler->SendSysMessage("[Auctionator] buyout: expected 1 (listings have a buyout) "
                    "or 0 (no buyout, bidding only).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            auctionator->config->sellerConfig.bidOnly = enabled == 1 ? 0 : 1;

            if (enabled == 1)
            {
                handler->SendSysMessage("[Auctionator] buyout: enabled - listings get a buyout again "
                    "(Auctionator.Seller.BidOnly = 0), and the start bid is derived from "
                    "Auctionator.Seller.BidStartModifier. Takes effect on the next seller run.");
                auctionator->logInfo("buyout: enabled (Auctionator.Seller.BidOnly = 0)");
            }
            else
            {
                handler->SendSysMessage("[Auctionator] buyout: disabled - listings have no buyout "
                    "(Auctionator.Seller.BidOnly = 1), so they can only be won by bidding. "
                    "Takes effect on the next seller run.");
                auctionator->logInfo("buyout: disabled (Auctionator.Seller.BidOnly = 1)");
            }

            return true;
        }

        // .auctionator maxitemlevel <value|off>
        //
        // Limit for the automatic seller; "off" (or 0) removes it. Only the running
        // configuration changes - the panel persists the same value in the option file.
        static bool CommandSetMaxItemLevel(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() != 1)
            {
                handler->SendSysMessage("[Auctionator] maxitemlevel: usage <value|off> (off or 0 = no limit)");
                return true;
            }

            uint32 maxItemLevel = 0;
            std::string const value = params[0];
            if (value != "off" && !TryParseUInt32(value, maxItemLevel))
            {
                handler->SendSysMessage("[Auctionator] maxitemlevel: expected a number or 'off' (0 = no limit).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            auctionator->config->sellerConfig.maxItemLevel = maxItemLevel;

            if (maxItemLevel == 0)
            {
                handler->SendSysMessage("[Auctionator] maxitemlevel: no limit (Auctionator.Seller.MaxItemLevel = 0). "
                    "Recipes whose crafted item cannot be resolved stay unlisted. Takes effect on the next seller run.");
                auctionator->logInfo("maxitemlevel: no limit (Auctionator.Seller.MaxItemLevel = 0)");
            }
            else
            {
                handler->SendSysMessage("[Auctionator] maxitemlevel: " + std::to_string(maxItemLevel)
                    + " (Auctionator.Seller.MaxItemLevel). The automatic seller stops listing items above it, and judges "
                      "a recipe by the level of the item it makes. Takes effect on the next seller run.");
                auctionator->logInfo("maxitemlevel: " + std::to_string(maxItemLevel) + " (Auctionator.Seller.MaxItemLevel)");
            }

            return true;
        }

        static bool CommandBidOnOwn(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] bidonown: Need to specify 1 (on) or 0 (off).");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] bidonown: usage <0|1>");
                return true;
            }

            uint32 bidOnOwn = 0;
            if (!TryParseUInt32(params[0], bidOnOwn)) {
                handler->SendSysMessage("[Auctionator] bidonown: Need to specify 1 (on) or 0 (off).");
                return true;
            }

            if (bidOnOwn == 1) {
                auctionator->config->bidOnOwn = 1;
                handler->SendSysMessage("[Auctionator] bidonown: Bid on own enabled.");
            } else if (bidOnOwn == 0) {
                auctionator->config->bidOnOwn = 0;
                handler->SendSysMessage("[Auctionator] bidonown: Bid on own disabled.");
            } else {
                // Anything else used to be accepted silently, leaving the flag untouched.
                handler->SendSysMessage("[Auctionator] bidonown: only 1 (on) or 0 (off) are valid; nothing changed.");
            }

            return true;
        }

        static bool CommandBidsPerCycle(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify number of items to bid on.");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] bidspercycle: usage <value>");
                return true;
            }

            uint32 bidsPerCycle = 0;
            if (!TryParseUInt32(params[0], bidsPerCycle)) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify a number.");
                return true;
            }

            if (bidsPerCycle > MaxBidderPurchasesPerCycle)
            {
                handler->SendSysMessage("[Auctionator] bidspercycle: capped at "
                    + std::to_string(MaxBidderPurchasesPerCycle) + " purchases per cycle.");
                bidsPerCycle = MaxBidderPurchasesPerCycle;
            }

            auctionator->config->allianceBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->hordeBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->neutralBidder.maxPerCycle = bidsPerCycle;

            handler->SendSysMessage("[Auctionator] bidspercycle: Set bids per cycle to " + std::to_string(bidsPerCycle));

            return true;
        }

        static bool CommandAuctionsPerCycle(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: Need to specify a number.");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: usage <value>");
                return true;
            }

            uint32 auctionsPerCycle = 0;
            if (!TryParseUInt32(params[0], auctionsPerCycle)) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: Need to specify a number.");
                return true;
            }
            auctionator->config->sellerConfig.auctionsPerRun = auctionsPerCycle;
            handler->SendSysMessage("[Auctionator] auctionspercycle: Set auctions per cycle to "
                + std::to_string(auctionsPerCycle));

            return true;
        }
};

void AddAuctionatorCommands()
{
    new AuctionatorCommands();
}
