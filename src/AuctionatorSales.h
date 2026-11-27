
#ifndef AUCTIONATOR_SALES_H
#define AUCTIONATOR_SALES_H

#include "AuctionatorBase.h"
#include "AuctionHouseMgr.h"

#include <string>

//
// Record of which auctions this module created, and of how they ended:
//
//   mod_auctionator_auction(auction_id, owner_guid, item_entry, created_at)
//   mod_auctionator_sale(
//       id, auction_id, item_entry, item_count, house_id, seller_guid, seller_is_bot,
//       module_listing, buyer_guid, price, startbid, buyout, deposit, cut, is_buyout, sold_at)
//
// The core keeps no record of a finished auction: `characters.auctionhouse` holds the
// live auctions only (its row is deleted together with the settlement), and the core's
// own `log_money` row is written for sales of 500 gold and up only. A module listing
// therefore became untraceable the moment a player won it, at every price below that -
// which matters here, because the sale gold is swallowed by the mail script (gold sink)
// and the sale mail itself is deleted instead of sent.
//
// Rows are written from the module's "auction successful" hook, i.e. for both ways a
// listing can end up sold: a buyout (WorldSession::HandleAuctionPlaceBid) and a winning
// bid at expiry (AuctionHouseObject::Update).
//
// Both tables come from data/sql/db-characters/updates/2026_09_27_00_sale_log.sql and
// 2026_09_27_01_auction_registry.sql.
//
class AuctionatorSales : public AuctionatorBase
{
    public:
        AuctionatorSales();

        // Remembers that this module created the given auction, so that its sale can later be
        // attributed by id instead of guessed at. Appends to the caller's transaction: the row
        // has to land together with the auctionhouse row it describes.
        void RememberCreated(AuctionEntry const* auction, CharacterDatabaseTransaction trans);

        // Records one sale of a module-created listing and ignores everything else
        // (player auctions belong to the core's own accounting, not to this module).
        void RecordSale(AuctionEntry const* auction);

    private:
        // Probed once per process and cached: the answer cannot change while the worldserver is
        // running (applying the SQL update is a restart anyway), and the hook that calls this
        // runs on every single sale.
        bool TableIsReady(std::string const& table, std::string const& updateFile);

        // Is this auction one the module created? Answered from mod_auctionator_auction.
        //
        // This used to be inferred from `deposit = 0`, which is not a marker at all: the core
        // computes a player's deposit as `AH_MINIMUM_DEPOSIT * Rate.Auction.Deposit` (and uses
        // that same value as a floor), so a realm configured with Rate.Auction.Deposit = 0 gives
        // player listings a deposit of 0 too - and every one of them was recorded as if the
        // module had created it. See AuctionHouseMgr::GetAuctionDeposit().
        bool IsRegistered(uint32 auctionId);
};

#endif //AUCTIONATOR_SALES_H
