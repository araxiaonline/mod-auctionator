
#ifndef AUCTIONATORBASE_H
#define AUCTIONATORBASE_H

#include "AuctionatorBase.h"
#include "Log.h"

#ifdef TRINITYCORE_LOG_H

#define LOG_TRACE(filterType__, message__, ...) \
    TC_LOG_TRACE(filterType__, "{}", message__, ## __VA_ARGS__)

#define LOG_DEBUG(filterType__, message__, ...) \
    TC_LOG_DEBUG(filterType__, "{}", message__, ## __VA_ARGS__)

#define LOG_INFO(filterType__, message__, ...)  \
    TC_LOG_INFO(filterType__, "{}", message__, ## __VA_ARGS__)

#define LOG_WARN(filterType__, message__, ...)  \
    TC_LOG_WARN(filterType__, "{}", message__, ## __VA_ARGS__)

#define LOG_ERROR(filterType__, message__, ...) \
    TC_LOG_ERROR(filterType__, "{}", message__, ## __VA_ARGS__)

#define LOG_FATAL(filterType__, message__, ...) \
    TC_LOG_FATAL(filterType__, "{}", message__, ## __VA_ARGS__)

enum AuctionHouseType
{
    AUCTIONHOUSE_NEUTRAL = 0,
    AUCTIONHOUSE_ALLIANCE = 1,
    AUCTIONHOUSE_HORDE = 2
};

struct AuctionEntry : AuctionPosting
{

};

#endif

class AuctionatorBase
{
    private:
        std::string logPrefix = "[Auctionator] ";
    public:
        void logDebug(std::string message) {
            std::string output = logPrefix + message;
            LOG_DEBUG("auctionator", output);
        }

        void logError(std::string message) {
            std::string output = logPrefix + message;
            LOG_ERROR("auctionator", output);
        }

        void logInfo(std::string message) {
            std::string output = logPrefix + message;
            LOG_INFO("auctionator", output);
        }

        void logTrace(std::string message) {
            std::string output = logPrefix + message;
            LOG_TRACE("auctionator", output);
        }

        void SetLogPrefix(std::string prefix)
        {
            logPrefix = prefix;
        }
};

#endif
