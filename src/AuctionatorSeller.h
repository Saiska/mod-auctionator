
#ifndef AUCTIONATORSELLER_H
#define AUCTIONATORSELLER_H

#include "Auctionator.h"
#include "AuctionHouseMgr.h"
#include <array>
#include <unordered_map>
#include <vector>

struct CachedItem {
    uint32 entry;
    std::string name;
    uint32 basePrice;
    uint32 stackable;
    uint32 quality;
    uint32 marketPrice;
    uint32 maxCount;
    uint32 itemClass;
};

class AuctionatorSeller : public AuctionatorBase
{
    private:
        Auctionator* nator;
        uint32 auctionHouseId;
        AuctionHouseObject* ahMgr;

        void PlaceAuction(const CachedItem& item,
                          std::unordered_map<uint32, uint32>& currentCounts,
                          uint32 houseId);
        uint32 SellByTargetShare(
            const std::array<std::vector<const CachedItem*>, 17>& classBuckets,
            std::unordered_map<uint32, uint32>& currentCounts,
            uint32 maxCount, uint32 houseId);

    public:
        AuctionatorSeller(Auctionator* natorParam, uint32 auctionHouseIdParam);
        ~AuctionatorSeller();
        void LetsGetToIt(uint32 maxCount, uint32 houseId);
        uint32 GetRandomNumber(uint32 min, uint32 max);
};

#endif  //AUCTIONATORSELLER_H
