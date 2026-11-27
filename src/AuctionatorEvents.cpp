
#include "AuctionatorEvents.h"
#include "AuctionatorBidder.h"
#include "AuctionatorSeller.h"
#include "AuctionatorMarketData.h"
#include "Log.h"
#include "Timer.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <string>

namespace
{
    // Event ids. They are also the keys of eventToFunction/eventHandlers.
    enum AuctionatorEventId : uint32
    {
        AUCTIONATOR_EVENT_ALLIANCE_BIDDER = 1,
        AUCTIONATOR_EVENT_HORDE_BIDDER    = 2,
        AUCTIONATOR_EVENT_NEUTRAL_BIDDER  = 3,
        AUCTIONATOR_EVENT_ALLIANCE_SELLER = 4,
        AUCTIONATOR_EVENT_HORDE_SELLER    = 5,
        AUCTIONATOR_EVENT_NEUTRAL_SELLER  = 6,
        AUCTIONATOR_EVENT_MARKET_IMPORT   = 7,
        AUCTIONATOR_EVENT_MARKET_SCAN     = 8,
        AUCTIONATOR_EVENT_COUNT           = 8
    };
}

AuctionatorEvents::AuctionatorEvents(AuctionatorConfig* auctionatorConfig)
{
    SetLogPrefix("[AuctionatorEvents] ");
    events = EventMap();
    config = auctionatorConfig;
    InitializeEvents();
}

void AuctionatorEvents::InitializeEvents()
{
    if (!config) {
        logError("AuctionatorEvents::InitializeEvents skipped: config is null.");
        return;
    }

    logInfo("Initializing events");

    eventToFunction = {
            {AUCTIONATOR_EVENT_ALLIANCE_BIDDER, "AllianceBidder"},
            {AUCTIONATOR_EVENT_HORDE_BIDDER, "HordeBidder"},
            {AUCTIONATOR_EVENT_NEUTRAL_BIDDER, "NeutralBidder"},
            {AUCTIONATOR_EVENT_ALLIANCE_SELLER, "AllianceSeller"},
            {AUCTIONATOR_EVENT_HORDE_SELLER, "HordeSeller"},
            {AUCTIONATOR_EVENT_NEUTRAL_SELLER, "NeutralSeller"},
            {AUCTIONATOR_EVENT_MARKET_IMPORT, "MarketImport"},
            {AUCTIONATOR_EVENT_MARKET_SCAN, "MarketScan"}
        };

    eventHandlers = {
            {AUCTIONATOR_EVENT_ALLIANCE_BIDDER, &AuctionatorEvents::EventAllianceBidder},
            {AUCTIONATOR_EVENT_HORDE_BIDDER, &AuctionatorEvents::EventHordeBidder},
            {AUCTIONATOR_EVENT_NEUTRAL_BIDDER, &AuctionatorEvents::EventNeutralBidder},
            {AUCTIONATOR_EVENT_ALLIANCE_SELLER, &AuctionatorEvents::EventAllianceSeller},
            {AUCTIONATOR_EVENT_HORDE_SELLER, &AuctionatorEvents::EventHordeSeller},
            {AUCTIONATOR_EVENT_NEUTRAL_SELLER, &AuctionatorEvents::EventNeutralSeller},
            {AUCTIONATOR_EVENT_MARKET_IMPORT, &AuctionatorEvents::EventMarketImport},
            {AUCTIONATOR_EVENT_MARKET_SCAN, &AuctionatorEvents::EventMarketScan}
        };

    // uint16 matches EventMap's event id type.
    for (uint16 eventId = 1; eventId <= AUCTIONATOR_EVENT_COUNT; ++eventId)
    {
        if (IsEventEnabled(eventId))
        {
            // The market events want an early first run, so a fresh deployment (or a GM who just
            // turned one on) gets data instead of waiting out a multi-hour interval.
            bool const earlyFirstRun = eventId == AUCTIONATOR_EVENT_MARKET_IMPORT
                || eventId == AUCTIONATOR_EVENT_MARKET_SCAN;
            uint32 const firstDelayMinutes = earlyFirstRun
                ? 1
                : GetEventInterval(eventId).count();

            events.ScheduleEvent(eventId, std::chrono::minutes(firstDelayMinutes));
        }
    }
}

bool AuctionatorEvents::IsEventEnabled(uint16 currentEvent) const
{
    if (!config)
    {
        return false;
    }

    switch (currentEvent) {
        case AUCTIONATOR_EVENT_ALLIANCE_BIDDER:
            return config->allianceBidder.enabled != 0;
        case AUCTIONATOR_EVENT_HORDE_BIDDER:
            return config->hordeBidder.enabled != 0;
        case AUCTIONATOR_EVENT_NEUTRAL_BIDDER:
            return config->neutralBidder.enabled != 0;
        case AUCTIONATOR_EVENT_ALLIANCE_SELLER:
            return config->allianceSeller.enabled != 0;
        case AUCTIONATOR_EVENT_HORDE_SELLER:
            return config->hordeSeller.enabled != 0;
        case AUCTIONATOR_EVENT_NEUTRAL_SELLER:
            return config->neutralSeller.enabled != 0;
        case AUCTIONATOR_EVENT_MARKET_IMPORT:
            return !config->marketDataImportFile.empty();
        case AUCTIONATOR_EVENT_MARKET_SCAN:
            // 0 = only on demand: the GM command and the panel button still work, the timer
            // simply never fires.
            return config->marketDataScanIntervalMinutes > 0;
        default:
            return false;
    }
}

// A scheduled event may never fire immediately a second time: EventMap gives an
// event back for as long as its due time is <= the map's internal clock, so an
// interval of zero would make ExecuteEvents() loop on the same event forever and
// hang the world thread.
static std::chrono::minutes ClampEventInterval(uint32 minutes)
{
    return std::chrono::minutes(std::max<uint32>(1, minutes));
}

std::chrono::minutes AuctionatorEvents::GetEventInterval(uint16 currentEvent) const
{
    if (!config)
    {
        return std::chrono::minutes(1);
    }

    switch (currentEvent) {
        case AUCTIONATOR_EVENT_ALLIANCE_BIDDER:
            return ClampEventInterval(config->allianceBidder.cycleMinutes);
        case AUCTIONATOR_EVENT_HORDE_BIDDER:
            return ClampEventInterval(config->hordeBidder.cycleMinutes);
        case AUCTIONATOR_EVENT_NEUTRAL_BIDDER:
            return ClampEventInterval(config->neutralBidder.cycleMinutes);
        case AUCTIONATOR_EVENT_ALLIANCE_SELLER:
            return ClampEventInterval(config->allianceSeller.cycleMinutes);
        case AUCTIONATOR_EVENT_HORDE_SELLER:
            return ClampEventInterval(config->hordeSeller.cycleMinutes);
        case AUCTIONATOR_EVENT_NEUTRAL_SELLER:
            return ClampEventInterval(config->neutralSeller.cycleMinutes);
        case AUCTIONATOR_EVENT_MARKET_IMPORT:
            return ClampEventInterval(config->marketDataImportIntervalMinutes);
        case AUCTIONATOR_EVENT_MARKET_SCAN:
            return ClampEventInterval(config->marketDataScanIntervalMinutes);
        default:
            return std::chrono::minutes(1);
    }
}

void AuctionatorEvents::RescheduleEvent(uint16 currentEvent)
{
    if (IsEventEnabled(currentEvent))
    {
        events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
    }
}

bool AuctionatorEvents::SkipSharedHouseEvent(std::string const& what, uint32 houseId)
{
    if (!Auctionator::IsHouseHiddenBySharedAuctionHouse(houseId))
    {
        sharedHouseWarned.erase(what);
        return false;
    }

    // Without this check the seller would log "count is good, here we go" and only then be
    // refused by AuctionatorSeller, which reads like a bug. Reported once per event rather
    // than on every cycle: the default alliance/horde setup on a shared-house realm would
    // otherwise write this line every few minutes for as long as the realm runs.
    if (sharedHouseWarned.insert(what).second)
    {
        logWarn(what + " skipped for house " + std::to_string(houseId)
            + ": AllowTwoSide.Interaction.Auction is enabled, so only house 7 (neutral) is visible to players. "
              "Enable Auctionator.NeutralSeller / Auctionator.NeutralBidder and leave the 2/6 ones disabled. "
              "(Reported once; the event keeps being skipped.)");
    }

    return true;
}

void AuctionatorEvents::DispatchEvent(uint16 currentEvent)
{
    if (!config) {
        logError("AuctionatorEvents::DispatchEvent skipped: config is null.");
        return;
    }

    auto eventIt = eventToFunction.find(currentEvent);
    if (eventIt == eventToFunction.end()) {
        logError("Unknown event id: " + std::to_string(currentEvent));
        return;
    }

    auto handlerIt = eventHandlers.find(currentEvent);
    if (handlerIt == eventHandlers.end() || !handlerIt->second) {
        logError("No handler registered for event id: " + std::to_string(currentEvent));
        return;
    }

    logInfo("Executing event: " + eventIt->second);

    try {
        (this->*(handlerIt->second))();
    } catch (const std::exception& e) {
        // Still reschedule: an event that threw once (a database hiccup, a bad row) should
        // not silently drop out of the schedule until the next enable/disable or restart.
        logError("Issue calling handler for event id " + std::to_string(currentEvent));
        logError(e.what());
        RescheduleEvent(currentEvent);
        return;
    }

    RescheduleEvent(currentEvent);
}

void AuctionatorEvents::ExecuteEvents()
{
    //
    // Seven events exist, so a single tick can only legitimately dispatch a handful of
    // them. The cap is a safety net for a schedule that somehow became due immediately
    // again (see ClampEventInterval above): it bounds the work done in one tick and
    // reports the problem instead of spinning the world thread.
    //
    uint32 const maxEventsPerTick = 64;

    uint16 currentEvent = events.ExecuteEvent();
    for (uint32 dispatched = 0; currentEvent != 0; ++dispatched)
    {
        if (dispatched >= maxEventsPerTick)
        {
            logError("event dispatch cap (" + std::to_string(maxEventsPerTick)
                + ") reached in one tick; the event schedule looks broken, aborting this tick.");
            return;
        }

        DispatchEvent(currentEvent);
        currentEvent = events.ExecuteEvent();
    }
}

void AuctionatorEvents::Update(uint32 deltaMilliseconds)
{
    events.Update(deltaMilliseconds);
    ExecuteEvents();
}

void AuctionatorEvents::ResyncSchedule()
{
    if (!config) {
        return;
    }

    //
    // Called after ".auctionator enable/disable": events are only scheduled from the
    // flags at startup, so without this a flag turned on at runtime never ran.
    //
    for (uint16 eventId = 1; eventId <= AUCTIONATOR_EVENT_COUNT; ++eventId)
    {
        if (!IsEventEnabled(eventId))
        {
            events.CancelEvent(eventId);
            continue;
        }

        // Not scheduled yet: run soon so the GM sees the effect, then the normal interval
        // takes over through RescheduleEvent().
        if (!events.HasTimeUntilEvent(eventId))
        {
            events.ScheduleEvent(eventId, std::chrono::minutes(1));
            logInfo("event " + std::to_string(eventId) + " enabled, first run in about a minute");
        }
    }
}

void AuctionatorEvents::CancelAllEvents()
{
    for (uint16 eventId = 1; eventId <= AUCTIONATOR_EVENT_COUNT; ++eventId)
    {
        events.CancelEvent(eventId);
    }
}

void AuctionatorEvents::SetPlayerGuid(ObjectGuid playerGuid)
{
    auctionatorGuid = playerGuid;
}

void AuctionatorEvents::RunBidder(std::string const& faction, uint32 houseId)
{
    if (!config) {
        logError(faction + " bidder skipped: config is null.");
        return;
    }

    if (SkipSharedHouseEvent(faction + " bidder", houseId)) {
        return;
    }

    logInfo("Starting " + faction + " Bidder");
    AuctionatorBidder bidder = AuctionatorBidder(houseId, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::RunSeller(std::string const& faction, uint32 houseId, AuctionatorHouseConfig const& houseConfig)
{
    // GetAuctionHouse() stays null until Initialize() succeeded.
    if (!config || !Auctionator::getInstance()->GetAuctionHouse(houseId)) {
        logError(faction + " auction house is not initialized.");
        return;
    }

    if (SkipSharedHouseEvent(faction + " seller", houseId)) {
        return;
    }

    uint32 const auctionCount = Auctionator::getInstance()->CountAuctions(houseId);
    if (auctionCount < houseConfig.maxAuctions) {
        logInfo(
            faction + " count is good, here we go: "
            + std::to_string(auctionCount)
            + " of " + std::to_string(houseConfig.maxAuctions)
        );

        AuctionatorSeller seller = AuctionatorSeller(Auctionator::getInstance());
        seller.LetsGetToIt(config->sellerConfig.auctionsPerRun, houseId);
    } else {
        logInfo(faction + " count over max: " + std::to_string(auctionCount));
    }
}

void AuctionatorEvents::EventAllianceBidder()
{
    RunBidder("Alliance", (uint32)AuctionHouseId::Alliance);
}

void AuctionatorEvents::EventHordeBidder()
{
    RunBidder("Horde", (uint32)AuctionHouseId::Horde);
}

void AuctionatorEvents::EventNeutralBidder()
{
    RunBidder("Neutral", (uint32)AuctionHouseId::Neutral);
}

void AuctionatorEvents::EventAllianceSeller()
{
    if (config) {
        RunSeller("Alliance", (uint32)AuctionHouseId::Alliance, config->allianceSeller);
    }
}

void AuctionatorEvents::EventHordeSeller()
{
    if (config) {
        RunSeller("Horde", (uint32)AuctionHouseId::Horde, config->hordeSeller);
    }
}

void AuctionatorEvents::EventNeutralSeller()
{
    if (config) {
        RunSeller("Neutral", (uint32)AuctionHouseId::Neutral, config->neutralSeller);
    }
}

void AuctionatorEvents::PruneMarketData()
{
    //
    // The market table keeps one row per item and scan and nothing else ever removed them, so
    // a realm on a short scan interval grew it without bound unless a GM remembered
    // ".auctionator marketprune".
    //
    // Never automatic when either setting is 0: RetentionDays = 0 is "keep everything", and
    // MaxAgeDays = 0 means the readers use a row however old it is, so no row is safe to drop.
    // Rows the readers would still use (younger than MaxAgeDays) are never pruned either.
    //
    if (!config || config->marketDataRetentionDays == 0 || config->marketDataMaxAgeDays == 0) {
        return;
    }

    // At most once an hour: the prune counts before it deletes, and scan_datetime alone is
    // not indexed.
    uint32 const nowMs = getMSTime();
    if (lastMarketPruneMs != 0 && getMSTimeDiff(lastMarketPruneMs, nowMs) < HOUR * IN_MILLISECONDS) {
        return;
    }

    lastMarketPruneMs = nowMs;

    AuctionatorMarketData marketData;
    marketData.PruneOlderThan(std::max(config->marketDataRetentionDays, config->marketDataMaxAgeDays));
}

void AuctionatorEvents::EventMarketImport()
{
    if (!config || config->marketDataImportFile.empty()) {
        return;
    }

    logInfo("Starting market data import");

    AuctionatorMarketData marketData;
    marketData.ImportFromFile(
        config->marketDataImportFile,
        config->marketDataImportSource,
        config->marketDataImportMaxRows,
        false
    );

    PruneMarketData();
}

void AuctionatorEvents::EventMarketScan()
{
    if (!config || config->marketDataScanIntervalMinutes == 0) {
        return;
    }

    logInfo("Starting market scan (auction house sample)");

    // Same rule as the GM command and the panel button: ScanExcludeSelf decides whether the
    // auctionator's own listings take part in the sample.
    AuctionatorMarketData marketData;
    marketData.ScanAuctionHouse(config->marketDataScanExcludeSelf != 0 ? config->characterGuid : 0);

    PruneMarketData();
}
