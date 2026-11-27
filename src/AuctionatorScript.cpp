
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "Log.h"
#include "Auctionator.h"
#include "AuctionatorSales.h"
#include "ObjectAccessor.h"
#include "Player.h"

class AuctionatorWorldScript : public WorldScript
{
public:
    AuctionatorWorldScript() : WorldScript("Auctionator_WorldScript") { }

    void OnStartup() override
    {
        Auctionator* auctionator = Auctionator::getInstance();
        LOG_INFO("server.loading", "[Auctionator]: Auctionator initializing...");
        if (auctionator->config->isEnabled) {
            LOG_INFO("server.loading", "[Auctionator]: Auctionator enabled.");
        } else {
            LOG_INFO("server.loading", "[Auctionator]: Auctionator disabled.");
        }

        //
        // The bidder is a gold faucet by design: the module holds no money, so nothing is
        // escrowed for its bids/buyouts while the core pays the seller from newly created
        // gold (and the item it wins is sunk by the mail recycling). Warn once at startup
        // so enabling it is never a surprise after a config edit.
        //
        if (auctionator->config->allianceBidder.enabled
            || auctionator->config->hordeBidder.enabled
            || auctionator->config->neutralBidder.enabled)
        {
            LOG_WARN("server.loading", "[Auctionator]: a bidder is enabled. Its bids and buyouts are NOT escrowed (the module has no funds); the core pays the seller with newly created gold, so the realm's money supply grows with every purchase and the item the bot wins is destroyed by the mail recycling.");
        }
    }

    // Runs before StopDB() closes the database pools, which matters because
    // ~WorldSession() writes account.totaltime / account.online.
    void OnShutdown() override
    {
        Auctionator::getInstance()->Shutdown();
    }
};

class AuctionatorHouseScript : public AuctionHouseScript
{
    public:
        AuctionatorHouseScript() : AuctionHouseScript("AuctionatorHouseScript") {}

        void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(
                AuctionHouseMgr*,
                AuctionEntry* auction,
                Player* owner,
                uint32& /*owner_accId*/,
                uint32& /*profit*/,
                bool& sendNotification,
                bool& updateAchievementCriteria,
                bool& /*sendMail*/
            ) override
        {
            // Auctionator::getInstance() never returns null (it is a function local static).
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator->config || !auction) {
                return;
            }

            //
            // Sold. This is the only moment the sale exists: the core deletes the
            // `auctionhouse` row together with the settlement and writes a `log_money` row
            // for sales of 500 gold and up only, so without this the sale of a module
            // listing would be gone for good - including which character bought it and for
            // how much. The recorder ignores player auctions (see AuctionatorSales).
            //
            AuctionatorSales().RecordSale(auction);

            //
            // The auctionator character is normally offline, in which case the core takes the
            // offline branch (sAchievementMgr->UpdateAchievementCriteriaForOfflinePlayer())
            // and this hook never sees an `owner` pointer at all - testing `owner` here used to
            // make the whole suppression dead code. Test the auction's owner guid instead: a
            // bot sale must not accumulate achievement progress, and there is nobody to
            // notify. If the configured character is online (a human is playing it), leave
            // everything alone - that player deserves the notification and the progress.
            //
            if (!owner && auction->owner.GetCounter() == auctionator->config->characterGuid)
            {
                sendNotification = false;
                updateAchievementCriteria = false;
            }
        }

        void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(
                AuctionHouseMgr* ,
                AuctionEntry* auction,
                Player* owner,
                uint32& /*owner_accId*/,
                bool& sendNotification,
                bool& /*sendMail*/
            ) override
        {
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator->config || !auction) {
                return;
            }

            // Same guid-based test as above: an offline owner is never notified by this core
            // revision, so this is a safety net for the configured (bot) character only.
            if (!owner && auction->owner.GetCounter() == auctionator->config->characterGuid)
                sendNotification = false;
        }

        // The core only sends the in-game "you have been outbid" notification when it has
        // a newBidder object; the module's buyout passes nullptr (it has no player object
        // for the bot), so the notification is sent from here instead. The mail itself
        // (and the refund it carries) is unaffected.
        void OnBeforeAuctionHouseMgrSendAuctionOutbiddedMail(
                AuctionHouseMgr* /*auctionHouseMgr*/,
                AuctionEntry* auction,
                Player* oldBidder,
                uint32& /*oldBidder_accId*/,
                Player* newBidder,
                uint32& newPrice,
                bool& /*sendNotification*/,
                bool& /*sendMail*/
            ) override
        {
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator->config || !auction || !oldBidder || newBidder) {
                return;
            }

            if (oldBidder->GetSession())
                oldBidder->GetSession()->SendAuctionBidderNotification(
                    (uint32)auction->GetHouseId(),
                    auction->Id,
                    ObjectGuid::Create<HighGuid::Player>(auctionator->config->characterGuid),
                    newPrice,
                    auction->GetAuctionOutBid(),
                    auction->item_template);
        }

        void OnBeforeAuctionHouseMgrUpdate() override
        {
            Auctionator::getInstance()->Update();
        }

};

class AuctionatorMailScript : public MailScript
{
public:
    AuctionatorMailScript() : MailScript("AuctionatorMailScript") { }

    void OnBeforeMailDraftSendMailTo(MailDraft* /*mailDraft*/, MailReceiver const& receiver, MailSender const& sender, MailCheckMask& /*checked*/, uint32& /*deliver_delay*/, uint32& /*custom_expiration*/, bool& deleteMailItemsFromDB, bool& sendMail) override
    {
        Auctionator* auctionator = Auctionator::getInstance();
        if (!auctionator->config) {
            return;
        }

        if (receiver.GetPlayerGUIDLow() != auctionator->config->characterGuid) {
            return;
        }

        if (sender.GetMailMessageType() != MAIL_AUCTION) {
            return;
        }

        //
        // Every MAIL_AUCTION message addressed to the configured character is recycled, not
        // just the "sold" one:
        //
        //   * AUCTION_SUCCESSFUL carries the sale money (the gold sink), and
        //   * AUCTION_EXPIRED carries the *item* of a listing nobody bought
        //     (AuctionHouseMgr::SendAuctionExpiredMail() attaches it with AddItem()).
        //
        // The expired one has to be recycled as well, because the configured character is
        // never logged in and the core only ever clears a mailbox when the owner logs in.
        // Without this the module leaked one dead mail (plus its item_instance row) per
        // unsold listing, forever: the mail table grew without bound, and once the 100 mail
        // cap was reached the core silently dropped the next expiry - destroying the item
        // anyway, but invisibly.
        //
        // Hard invariant: never destroy a real player's mail.
        //
        // If the receiver of this auction mail is online, then a human (or a bot system)
        // is playing the character configured as the auctionator, and recycling that mail
        // would delete its items and gold. Step aside and let the mail through.
        //
        // The dedicated auctionator character is never logged in, so the normal setup
        // keeps recycling auction mail (items and sale gold) as intended.
        //
        ObjectGuid const receiverGuid = ObjectGuid::Create<HighGuid::Player>(receiver.GetPlayerGUIDLow());
        if (receiver.GetPlayer() || ObjectAccessor::FindConnectedPlayer(receiverGuid)) {
            auctionator->logWarn("character "
                + std::to_string(receiver.GetPlayerGUIDLow())
                + " is configured as the auctionator but is currently online; keeping its auction mail instead of recycling it.");
            return;
        }

        // deleteMailItemsFromDB also drops the expired listing's item (Item::SaveToDB()'s
        // ITEM_REMOVED branch), and sendMail = false drops the mail row itself.
        deleteMailItemsFromDB = true;
        sendMail = false;
    }
};

void AddAuctionatorScripts()
{
    new AuctionatorWorldScript();
    new AuctionatorHouseScript();
    new AuctionatorMailScript();
};
