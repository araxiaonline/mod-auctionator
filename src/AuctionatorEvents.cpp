
#include "AuctionatorEvents.h"
#include "AuctionatorBidder.h"
#include "AuctionatorSeller.h"
#include "AuctionatorMarketData.h"
#include "Log.h"
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

bool AuctionatorEvents::SkipSharedHouseEvent(char const* what, uint32 houseId) const
{
    if (houseId == (uint32)AuctionHouseId::Neutral || !Auctionator::UsesSharedNeutralAuctionHouse())
    {
        return false;
    }

    // One line per event per cycle. Without this check the seller would log "count is good,
    // here we go" and only then be refused by AuctionatorSeller, which reads like a bug.
    logError(std::string(what ? what : "event") + " skipped for house " + std::to_string(houseId)
        + ": AllowTwoSide.Interaction.Auction is enabled, so only house 7 (neutral) is visible to players. "
          "Enable Auctionator.NeutralSeller / Auctionator.NeutralBidder and leave the 2/6 ones disabled.");
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

void AuctionatorEvents::EventAllianceBidder()
{
    if (!config) {
        logError("Alliance bidder skipped: config is null.");
        return;
    }

    if (SkipSharedHouseEvent("Alliance bidder", (uint32)AuctionHouseId::Alliance)) {
        return;
    }

    logInfo("Starting Alliance Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Alliance, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventHordeBidder()
{
    if (!config) {
        logError("Horde bidder skipped: config is null.");
        return;
    }

    if (SkipSharedHouseEvent("Horde bidder", (uint32)AuctionHouseId::Horde)) {
        return;
    }

    logInfo("Starting Horde Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Horde, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventNeutralBidder()
{
    if (!config) {
        logError("Neutral bidder skipped: config is null.");
        return;
    }

    if (SkipSharedHouseEvent("Neutral bidder", (uint32)AuctionHouseId::Neutral)) {
        return;
    }

    logInfo("Starting Neutral Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Neutral, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventAllianceSeller()
{
    // GetAuctionHouse() stays null until Initialize() resolved the house maps.
    if (!config || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Alliance)) {
        logError("Alliance auction house is not initialized.");
        return;
    }

    if (SkipSharedHouseEvent("Alliance seller", (uint32)AuctionHouseId::Alliance)) {
        return;
    }

    uint32 const auctionCountAlliance = Auctionator::getInstance()->CountAuctions((uint32)AuctionHouseId::Alliance);
    if (auctionCountAlliance < config->allianceSeller.maxAuctions) {
        logInfo(
            "Alliance count is good, here we go: "
            + std::to_string(auctionCountAlliance)
            + " of " + std::to_string(config->allianceSeller.maxAuctions)
        );

        AuctionatorSeller sellerAlliance = AuctionatorSeller(Auctionator::getInstance());
        sellerAlliance.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Alliance
        );
    } else {
        logInfo("Alliance count over max: " + std::to_string(auctionCountAlliance));
    }
}

void AuctionatorEvents::EventHordeSeller()
{
    if (!config || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Horde)) {
        logError("Horde auction house is not initialized.");
        return;
    }

    if (SkipSharedHouseEvent("Horde seller", (uint32)AuctionHouseId::Horde)) {
        return;
    }

    uint32 const auctionCountHorde = Auctionator::getInstance()->CountAuctions((uint32)AuctionHouseId::Horde);
    if (auctionCountHorde < config->hordeSeller.maxAuctions) {
        logInfo(
            "Horde count is good, here we go: "
            + std::to_string(auctionCountHorde)
            + " of " + std::to_string(config->hordeSeller.maxAuctions)
        );

        AuctionatorSeller sellerHorde = AuctionatorSeller(Auctionator::getInstance());
        sellerHorde.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Horde
        );
    } else {
        logInfo("Horde count over max: " + std::to_string(auctionCountHorde));
    }
}

void AuctionatorEvents::EventNeutralSeller()
{
    if (!config || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Neutral)) {
        logError("Neutral auction house is not initialized.");
        return;
    }

    if (SkipSharedHouseEvent("Neutral seller", (uint32)AuctionHouseId::Neutral)) {
        return;
    }

    uint32 const auctionCountNeutral = Auctionator::getInstance()->CountAuctions((uint32)AuctionHouseId::Neutral);
    if (auctionCountNeutral < config->neutralSeller.maxAuctions) {
        logInfo(
            "Neutral count is good, here we go: "
            + std::to_string(auctionCountNeutral)
            + " of " + std::to_string(config->neutralSeller.maxAuctions)
        );

        AuctionatorSeller sellerNeutral = AuctionatorSeller(Auctionator::getInstance());
        sellerNeutral.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Neutral
        );
    } else {
        logInfo("Neutral count over max: " + std::to_string(auctionCountNeutral));
    }
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
}
