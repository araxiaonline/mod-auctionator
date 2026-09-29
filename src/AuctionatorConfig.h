
#ifndef AUCTIONATOR_CONFIG_H
#define AUCTIONATOR_CONFIG_H

#include "Common.h"
#include <string>

// Upper bound for Auctionator.*Bidder.MaxPerCycle. The bidder samples the house's id
// range up to (MaxPerCycle * 10) auctions wide, so an unbounded value would let the GM
// command overflow that window and pull an arbitrarily large id range per cycle.
constexpr uint32 MaxBidderPurchasesPerCycle = 1000;

struct AuctionatorHouseConfig
{
    uint32 enabled = 0;
    uint32 maxAuctions = 100;
    uint32 cycleMinutes = 1;
};

struct AuctionatorBidderConfig
{
    uint32 enabled = 0;
    uint32 cycleMinutes = 30;
    uint32 maxPerCycle = 1;
};

struct AuctionatorPriceMultiplierConfig
{
    float poor = 1.0f;
    float normal = 1.0f;
    float uncommon = 1.5f;
    float rare = 2.0f;
    float epic = 6.0f;
    float legendary = 10.0f;
};

struct AuctionatorSellerConfig
{
    // Base price for items that have neither a vendor BuyPrice nor market data.
    // It is scaled by the per quality seller multiplier.
    uint32 defaultPrice = 1000000;
    uint32 auctionsPerRun = 100;
    uint32 randomizeStackSize = 1;
    // Fraction of the buyout the starting bid may lose: the start bid is drawn between
    // buyout * (1 - bidStartModifier) and the buyout, never 0 (a start bid of 0 means "any
    // bid wins" to the core, so a player could take the auction for 1 copper). Keep this in
    // sync with the fallback in Auctionator::InitializeConfig() and with
    // conf/mod_auctionator.conf.dist.
    float bidStartModifier = 0.3f;

    // 1 = every listing this module creates is bid-only: no buyout at all, and the computed
    // price becomes the start bid. The core reads buyout 0 as "no buyout"
    // (HandleAuctionPlaceBid() checks `price < auction->buyout || auction->buyout == 0`), and
    // CreateAuction() only refuses a listing when bid and buyout are *both* 0, so this needs
    // no core change. bidStartModifier is ignored in this mode: there is no buyout to discount
    // from, and a random discount would only lower the reserve price.
    // 0 = normal behaviour. Keep in sync with the fallback in Auctionator::InitializeConfig()
    // and with conf/mod_auctionator.conf.dist.
    uint32 bidOnly = 0;

    // Bias the item selection towards entries that have fresh market data,
    // weighted by traded volume.
    uint32 preferMarketItems = 1;

    // item_template.VerifiedBuild = 1 normally means "not verified by the DB
    // content team", not "invalid item", so the filter is off by default.
    // Turning it on drops roughly 30% of the otherwise eligible items.
    uint32 excludeUnverifiedItems = 0;

    // Highest effective item level the automatic seller may list; 0 = no limit.
    // A recipe (class 9) is judged by the level of the item its taught spell creates, not by
    // its own ItemLevel, which is a placeholder on recipes. A recipe whose crafted item cannot
    // be resolved (enchanting formulas, inert tomes) is never listed, whatever this value is.
    // Automatic seller only: GM listings are unaffected.
    // Keep in sync with Auctionator::InitializeConfig() and conf/mod_auctionator.conf.dist.
    uint32 maxItemLevel = 0;

    // Price floor, applied to item_template.SellPrice (1.0 = never list below
    // what a vendor would pay). The ceiling is applied to the market average and only used
    // when market data is available; it is applied *before* the floor, so the floor always
    // wins and the bot never lists below what a vendor pays.
    float minPriceModifier = 1.0f;
    float maxPriceModifier = 2.0f;
};

class AuctionatorConfig
{
    public:
        bool isEnabled = false;
        uint32 characterId = 0;
        uint32 characterGuid = 0;

        AuctionatorHouseConfig hordeSeller;
        AuctionatorHouseConfig allianceSeller;
        AuctionatorHouseConfig neutralSeller;

        AuctionatorBidderConfig allianceBidder;
        AuctionatorBidderConfig hordeBidder;
        AuctionatorBidderConfig neutralBidder;

        AuctionatorPriceMultiplierConfig sellerMultipliers;
        AuctionatorPriceMultiplierConfig bidderMultipliers;

        AuctionatorSellerConfig sellerConfig;

        uint32 bidOnOwn = 0;

        // Market data older than this many days is ignored on both sides: the
        // seller falls back to item_template.BuyPrice and the bidder to its
        // vendor price cap. 0 disables the age check.
        uint32 marketDataMaxAgeDays = 14;

        // CSV import loop (see the "Market data import" section of the conf).
        std::string marketDataImportFile;
        std::string marketDataImportSource = "file";
        uint32 marketDataImportIntervalMinutes = 360;
        uint32 marketDataImportMaxRows = 50000;
        uint32 marketDataRetentionDays = 30;

        // How often the realm's own auction house is sampled into the market table
        // ("market scan", see AuctionatorMarketData::ScanAuctionHouse). 0 = never on a timer:
        // the GM command ".auctionator marketscan" (and the panel button) still works.
        uint32 marketDataScanIntervalMinutes = 0;

        // 1 = the auctionator character's own listings are left out of that sample, so the
        // "market" price is not a mirror of the bot's last listing. 0 = sample every listing,
        // which is what a realm whose house only holds the bot's own stock needs to get any
        // price at all (the panel and the command both report the owner split either way).
        uint32 marketDataScanExcludeSelf = 1;
};

#endif
