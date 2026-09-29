
#ifndef AUCTIONATOR_H
#define AUCTIONATOR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "ItemTemplate.h"
#include "AuctionatorConfig.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "EventMap.h"
// IsReady() dereferences the sWorld singleton, so the header is not
// self-sufficient without this.
#include "World.h"
#include "AuctionatorEvents.h"
#include "AuctionatorBase.h"

// Only used as a pointer member; the definition is needed by Auctionator.cpp.
class WorldSession;

struct AuctionatorItem
{
    uint32 itemId = 0;
    uint32 houseId = (uint32)AuctionHouseId::Neutral;
    uint32 bid = 0;
    uint32 buyout = 0;
    uint32 time = 172800;
    uint32 stackSize = 1;
    // Character that owns the auction and receives the sale money.
    // 0 = the configured Auctionator character, whose mail is swallowed by the
    // mail script (i.e. the gold is removed from the economy).
    uint32 ownerGuid = 0;
};

class Auctionator : public AuctionatorBase
{
    private:
        Auctionator();
        // The class owns the dummy WorldSession and the config and is only ever used
        // through the getInstance() singleton, so copying it would duplicate ownership
        // and double-free.
        Auctionator(Auctionator const&) = delete;
        Auctionator& operator=(Auctionator const&) = delete;
        bool initialized = false;
        // Always initialized: Initialize() may bail out early (no valid
        // CharacterId/CharacterGuid, missing world/auction manager) and neither the
        // destructor nor the callers may ever see indeterminate pointers.
        AuctionHouseObject* HordeAh = nullptr;
        AuctionHouseObject* AllianceAh = nullptr;
        AuctionHouseObject* NeutralAh = nullptr;
        AuctionHouseEntry const* HordeAhEntry = nullptr;
        AuctionHouseEntry const* AllianceAhEntry = nullptr;
        AuctionHouseEntry const* NeutralAhEntry = nullptr;
        WorldSession* session = nullptr;
        AuctionatorEvents events;
        // getMSTime() of the previous Update(), so the event map is fed the time that
        // really elapsed instead of the interval the caller is assumed to use.
        uint32 lastUpdateMs = 0;

    public:
        ~Auctionator();
        // trans: lets a caller batch many listings into one database transaction (the
        // seller does this per run). nullptr = own transaction, committed here.
        // Returns true only when the auction was really created, so callers can report
        // (and count) what actually happened instead of what they asked for.
        bool CreateAuction(AuctionatorItem newItem, CharacterDatabaseTransaction trans = nullptr);
        // The realm shares one auction house between both factions
        // (CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION). The core then stores *every*
        // auction with houseId = Neutral (see WorldSession::HandleAuctionSellItem) and
        // indexes it in the neutral search partition, so houses 2/6 must not be used:
        // their entries would end up in a partition no client ever queries, i.e. an
        // auction nobody can see or bid on while still occupying the house quota.
        static bool UsesSharedNeutralAuctionHouse();
        // includePlayerAuctions = false expires only auctions owned by the
        // configured auctionator character; true expires every auction of that house.
        void ExpireAllAuctions(uint32 houseId, bool includePlayerAuctions = false);
        // Locates one live auction by its id, or nullptr when no such auction is loaded.
        // Auctions live in the per-house objects and
        // CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION makes all three of them the same
        // object, so every house is probed instead of trusting a caller supplied one.
        AuctionEntry* FindAuction(uint32 auctionId);
        // Takes one auction down by handing it to the core's own expiry path, which mails
        // the item back to its owner (the mail script recycles the configured
        // auctionator's auction mail, so the gold sink still holds) and deletes the row.
        // Refused when the auction already has a bid, because expiring that one *sells*
        // it to the bidder instead of cancelling it - the exact opposite of "take it
        // down" - and when the auctionator does not own it, unless
        // includePlayerAuctions is set. Returns false and fills `error` when nothing was
        // delisted, so a caller can report the real outcome instead of a guess.
        bool DelistAuction(uint32 auctionId, bool includePlayerAuctions, std::string& error);
        // Rewrites the start bid and the buyout of one live auction, persists them and
        // refreshes the search index - without that last step a running realm keeps
        // showing (and selling at) the old price, because the searcher holds its own copy
        // of every auction. `buyout` of 0 means "no buyout" (pure auction).
        // Same bid/ownership restrictions as DelistAuction.
        bool RepriceAuction(uint32 auctionId, uint32 startbid, uint32 buyout,
            bool includePlayerAuctions, std::string& error);
        AuctionHouseObject* GetAuctionHouse(uint32 houseId);
        // Number of auctions of one house. It counts by AuctionEntry::houseId instead of
        // using the house object's size, because with
        // CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION all three houses share one object.
        uint32 CountAuctions(uint32 houseId);
        bool ValidateHouseState(uint32 houseId, AuctionHouseObject*& house, AuctionHouseEntry const*& entry) const;
        void Initialize();
        void InitializeConfig(ConfigMgr* configMgr);
        // Startup check of Auctionator.CharacterId / CharacterGuid against the login and
        // characters databases (a wrong value fails silently at runtime otherwise).
        void ValidateConfiguredCharacter();
        // Releases the dummy session. Called from OnShutdown(), i.e. while the database
        // pools are still alive (WorldSession's destructor writes to the login database).
        void Shutdown();
        AuctionatorConfig* config = nullptr;
        void Update();
        // Re-evaluates the event schedule from the current config flags, so that
        // ".auctionator enable/disable" takes effect without a restart.
        void ResyncEventSchedule();
        // Runtime master switch ("Auctionator.Enabled"). That option is only read at
        // startup, so without this a realm whose module was disabled in the conf could
        // only be started by restarting its worldserver. Flips the switch in place and
        // re-arms (or drops) the event schedule accordingly. Returns the state in effect
        // after the call; false when the module has no config yet.
        bool SetEnabled(bool enabled);
        bool IsReady() const
        {
            return initialized && config && session && sAuctionMgr && sWorld;
        }

        static float GetQualityMultiplier(AuctionatorPriceMultiplierConfig const& config, uint32 quality);

        static Auctionator* getInstance()
        {
            static Auctionator instance;
            return &instance;
        }
};

#endif
