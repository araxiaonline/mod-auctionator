
#include <iostream>
#include <string>
#include "ScriptMgr.h"
#include "Chat.h"
#include "Auctionator.h"

using namespace Acore::ChatCommands;

class AuctionatorCommands : public CommandScript
{
    public:
        AuctionatorCommands() : CommandScript("AuctionatorCommandScript")
        {
        }

    private:
        std::vector<ChatCommand> GetCommands() const override
        {
            return std::vector<ChatCommand>
            {
               {
                  "auctionator",
                  SEC_GAMEMASTER,
                  true,
                  &HandleCommandOptions,
                  ""
               }
            };
        }

        static bool HandleCommandOptions(ChatHandler* handler, const char* args)
        {
            gAuctionator->logInfo("Executing Auctionator!");

            const char* command = strtok((char*)args, " ");

            if(!command)
            {
                command = "help";
            }

            const char* param1 = strtok(NULL, " ");
            const char* param2 = strtok(NULL, " ");
            const char* param3 = strtok(NULL, " ");
            const char* param4 = strtok(NULL, " ");

            // Create an array of const char* items
            const char* commandParams[] = { param1, param2, param3, param4 };

            if(strlen(command) == 0) {
                return true;
            }

            std::string commandString(command);

            gAuctionator->logDebug("Executing command: " + commandString);

            if (commandString.compare("add") == 0) {
                gAuctionator->logInfo("Adding new Item for GM");
                uint32 auctionHouseId = std::stoi(param1);
                uint32 itemId = std::stoi(param2);
                uint32 price = std::stoi(param3);
                AddItemForBuyout(auctionHouseId, itemId, price, gAuctionator);
            } else if (commandString == "expireall") {
                CommandExpireAll(commandParams, handler, gAuctionator);
            } else if (commandString == "enable") {
                CommandEnableSeller(commandParams, handler, gAuctionator);
            } else if (commandString == "disable") {
                CommandDisableSeller(commandParams, handler, gAuctionator);
            } else if (commandString == "status") {
                ShowStatus(handler, gAuctionator);
            } else if (commandString == "help") {
                ShowHelp(handler);
                return true;
            }


            return true;
        }

        static void AddItemForBuyout(uint32 auctionHouseId, uint32 itemId, uint32 price, Auctionator* auctionator)
        {
            AuctionatorItem newItem;
            newItem.houseId = auctionHouseId;
            newItem.itemId = itemId;
            newItem.buyout = price;

            auctionator->CreateAuction(newItem, AUCTIONHOUSE_NEUTRAL);
        }

        static void ShowHelp(ChatHandler* handler)
        {
            std::string helpString(R"(
Auctionator Help:
disable ...
enable ...
add ...
expireall ...
status
help
            )");
            handler->SendSysMessage(helpString);
        }

        static void ShowStatus(ChatHandler* handler, Auctionator* auctionator)
        {
            std::string statusString = "[Auctionator] Status:\n\n";

            statusString += " Enabled: " + std::to_string(auctionator->config->isEnabled) + "\n\n";

            statusString += " Horde:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->hordeSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->hordeSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse(AUCTIONHOUSE_HORDE)->Getcount()) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->hordeBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->hordeBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->hordeBidder.maxPerCycle) + "\n";

            statusString += " Alliance:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->allianceSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->allianceSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse(AUCTIONHOUSE_ALLIANCE)->Getcount()) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->allianceBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->allianceBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->allianceBidder.maxPerCycle) + "\n";

            statusString += " Neutral:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->neutralSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->neutralSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse(AUCTIONHOUSE_NEUTRAL)->Getcount()) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->neutralBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->neutralBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->neutralBidder.maxPerCycle) + "\n";

            handler->SendSysMessage(statusString);
        }

        static bool CommandExpireAll(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] expireall: No Auction House Specified!");
                auctionator->logInfo("expireall: No Auction House Specified!");
                return true;
            }
            uint32 houseId = std::stoi(params[0]);

            handler->SendSysMessage("[Auctionator] Expiring all Auctions for house: "
                + std::to_string(houseId));
            auctionator->logInfo("expireall: Expiring all auctions for house: "
                + std::to_string(houseId));

            auctionator->ExpireAllAuctions(houseId);

            auctionator->logInfo("expireall: All auctions expired for house: "
                + std::to_string(houseId));

            return true;
        }

        static bool CommandEnableSeller(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] enable: No Auction House Specified! [hordeseller, allianceseller, neutralseller, all]");
                auctionator->logInfo("enable: No Auction House Specified!");
                return true;
            }

            std::string toEnable(params[0]);
            if (toEnable == "hordeseller") {
                auctionator->config->hordeSeller.enabled = 1;
                auctionator->logInfo("Horde seller enabled");
                return true;
            } else if (toEnable == "allianceseller") {
                auctionator->config->allianceSeller.enabled = 1;
                auctionator->logInfo("Alliance seller enabled");
                return true;
            } else if (toEnable == "neutralseller") {
                auctionator->config->neutralSeller.enabled = 1;
                auctionator->logInfo("Neutral seller enabled");
                return true;
            } else if (toEnable == "all") {
                auctionator->config->hordeSeller.enabled = 1;
                auctionator->config->allianceSeller.enabled = 1;
                auctionator->config->neutralSeller.enabled = 1;
                auctionator->config->hordeBidder.enabled = 1;
                auctionator->config->allianceBidder.enabled = 1;
                auctionator->config->neutralBidder.enabled = 1;
                auctionator->logInfo("All sellers enabled");
                return true;
            } else if (toEnable == "hordebidder") {
                auctionator->config->hordeBidder.enabled = 1;
                auctionator->logInfo("Horde bidder enabled");
                return true;
            } else if (toEnable == "alliancebidder") {
                auctionator->config->allianceBidder.enabled = 1;
                auctionator->logInfo("Alliance bidder enabled");
                return true;
            } else if (toEnable == "neutralbidder") {
                auctionator->config->neutralBidder.enabled = 1;
                auctionator->logInfo("Neutral bidder enabled");
                return true;
            }

            return true;
        }

        static bool CommandDisableSeller(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] disable: No Auction House Specified! [hordeseller, allianceseller, neutralseller, all]");
                auctionator->logInfo("disable: No Auction House Specified!");
                return true;
            }

            std::string toDisable(params[0]);
            if (toDisable == "hordeseller") {
                auctionator->config->hordeSeller.enabled = 0;
                auctionator->logInfo("Horde seller disabled");
                return true;
            } else if (toDisable == "allianceseller") {
                auctionator->config->allianceSeller.enabled = 0;
                auctionator->logInfo("Alliance seller disabled");
                return true;
            } else if (toDisable == "neutralseller") {
                auctionator->config->neutralSeller.enabled = 0;
                auctionator->logInfo("Neutral seller disabled");
                return true;
            } else if (toDisable == "all") {
                auctionator->config->hordeSeller.enabled = 0;
                auctionator->config->allianceSeller.enabled = 0;
                auctionator->config->neutralSeller.enabled = 0;
                auctionator->config->hordeBidder.enabled = 0;
                auctionator->config->allianceBidder.enabled = 0;
                auctionator->config->neutralBidder.enabled = 0;
                auctionator->logInfo("All sellers disabled");
                return true;
            } else if (toDisable == "hordebidder") {
                auctionator->config->hordeBidder.enabled = 0;
                auctionator->logInfo("Horde bidder disabled");
                return true;
            } else if (toDisable == "alliancebidder") {
                auctionator->config->allianceBidder.enabled = 0;
                auctionator->logInfo("Alliance bidder disabled");
                return true;
            } else if (toDisable == "neutralbidder") {
                auctionator->config->neutralBidder.enabled = 0;
                auctionator->logInfo("Neutral bidder disabled");
                return true;
            }
        }
};

void AddAuctionatorCommands()
{
    new AuctionatorCommands();
}
