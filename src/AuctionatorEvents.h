
#ifndef AUCTIONATOR_EVENTS_H
#define AUCTIONATOR_EVENTS_H

#include "AuctionatorBase.h"
#include "AuctionatorConfig.h"
#include "EventMap.h"
#include "ObjectMgr.h"
#include <string>
#include <unordered_map>
#include <unordered_set>

class AuctionatorEvents : public AuctionatorBase
{
    private:
        using EventHandler = void (AuctionatorEvents::*)();

        ObjectGuid auctionatorGuid;
        EventMap events;
        std::unordered_map<uint32, std::string> eventToFunction;
        std::unordered_map<uint32, EventHandler> eventHandlers;
        AuctionatorConfig* config = nullptr;

        void DispatchEvent(uint16 currentEvent);
        void RescheduleEvent(uint16 currentEvent);
        // Reads the config flag that decides whether an event may run.
        bool IsEventEnabled(uint16 currentEvent) const;
        std::chrono::minutes GetEventInterval(uint16 currentEvent) const;
        // True when the event targets an Alliance/Horde house on a realm that shares one
        // auction house between the factions, where those houses are invisible to players.
        // Never true for the neutral house. Warns the first time an event is skipped.
        bool SkipSharedHouseEvent(std::string const& what, uint32 houseId);
        // The events that already reported being skipped (see SkipSharedHouseEvent).
        std::unordered_set<std::string> sharedHouseWarned;
        // The three bidder and the three seller events only differ in the house they act on.
        void RunBidder(std::string const& faction, uint32 houseId);
        void RunSeller(std::string const& faction, uint32 houseId, AuctionatorHouseConfig const& houseConfig);
        // Automatic retention for the market table, run after the timed import and scan.
        void PruneMarketData();
        // getMSTime() of the last automatic prune; 0 = none yet.
        uint32 lastMarketPruneMs = 0;

    public:
        AuctionatorEvents() = default;
        explicit AuctionatorEvents(AuctionatorConfig* auctionatorConfig);
        void InitializeEvents();
        void EventAllianceBidder();
        void EventHordeBidder();
        void EventNeutralBidder();
        void EventAllianceSeller();
        void EventHordeSeller();
        void EventNeutralSeller();
        void EventMarketImport();
        void EventMarketScan();
        void ExecuteEvents();
        // deltaMilliseconds: is fed straight into EventMap, which counts in ms.
        void Update(uint32 deltaMilliseconds);
        // Schedules events that are enabled but not scheduled yet, and cancels the
        // ones that were disabled at runtime.
        void ResyncSchedule();
        // Drops every pending timer. Used by the runtime master switch: while the module
        // is stopped no event may keep ageing towards a due time, otherwise a long stop
        // would end in a burst of overdue runs the moment it is started again.
        void CancelAllEvents();
        void SetPlayerGuid(ObjectGuid playerGuid);
};

#endif
