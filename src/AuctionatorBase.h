
#ifndef AUCTIONATORBASE_H
#define AUCTIONATORBASE_H

#include "Log.h"
#include <string>
#include <utility>

class AuctionatorBase
{
    private:
        std::string logPrefix = "[Auctionator] ";

    public:
        // The message is always passed as an *argument* to a constant format string: a
        // runtime message used as the format string breaks on item names containing
        // braces (fmt throws or swallows the text).
        //
        // All five are const: logging does not change the object, and this keeps them
        // callable from const member functions such as AuctionatorBidder::CalculateBuyPrice.
        void logDebug(std::string const& message) const { LOG_DEBUG("auctionator", "{}{}", logPrefix, message); }
        void logError(std::string const& message) const { LOG_ERROR("auctionator", "{}{}", logPrefix, message); }
        void logInfo(std::string const& message) const { LOG_INFO("auctionator", "{}{}", logPrefix, message); }
        void logWarn(std::string const& message) const { LOG_WARN("auctionator", "{}{}", logPrefix, message); }
        void logTrace(std::string const& message) const { LOG_TRACE("auctionator", "{}{}", logPrefix, message); }

        void SetLogPrefix(std::string prefix)
        {
            logPrefix = std::move(prefix);
        }
};

#endif
