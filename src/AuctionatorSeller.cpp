#include "Auctionator.h"
#include "AuctionHouseMgr.h"
#include "AuctionatorSeller.h"
#include "Item.h"
#include "DatabaseEnv.h"
#include "PreparedStatement.h"
#include <algorithm>
#include <random>
#include "QueryResult.h"


AuctionatorSeller::AuctionatorSeller(Auctionator* natorParam, uint32 auctionHouseIdParam)
{
    SetLogPrefix("[AuctionatorSeller] ");
    nator = natorParam;
    auctionHouseId = auctionHouseIdParam;

    ahMgr = nator->GetAuctionMgr(auctionHouseId);
};

AuctionatorSeller::~AuctionatorSeller()
{
    // TODO: clean up
};

void AuctionatorSeller::LetsGetToIt(uint32 maxCount, uint32 houseId)
{
    std::string characterDbName = CharacterDatabase.GetConnectionInfo()->database;
    static std::vector<CachedItem> cachedItems = [characterDbName]() {
        std::vector<CachedItem> items;

        std::string cacheQuery = R"(
            SELECT
                it.entry, it.name, it.BuyPrice, it.stackable, it.quality
                , COALESCE(mp.average_price, 0) as average_price, aicconf.max_count
                , it.class
            FROM
                mod_auctionator_itemclass_config aicconf
                INNER JOIN item_template it ON
                    aicconf.class = it.class
                    AND aicconf.subclass = it.subclass
                    AND it.bonding != 1
                    AND (it.bonding >= aicconf.bonding OR it.bonding = 0)
                    AND it.VerifiedBuild != 1
                LEFT JOIN mod_auctionator_disabled_items dis ON it.entry = dis.item
                LEFT JOIN (
                    SELECT mp1.entry, mp1.average_price
                    FROM {}.mod_auctionator_market_price mp1
                    INNER JOIN (
                        SELECT entry, MAX(scan_datetime) as max_scan
                        FROM {}.mod_auctionator_market_price
                        GROUP BY entry
                    ) mp2 ON mp1.entry = mp2.entry AND mp1.scan_datetime = mp2.max_scan
                ) mp ON it.entry = mp.entry
            WHERE dis.item IS NULL
        )";

        QueryResult result = WorldDatabase.Query(cacheQuery, characterDbName, characterDbName);

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                CachedItem item;
                item.entry = fields[0].Get<uint32>();
                item.name = fields[1].Get<std::string>();
                item.basePrice = fields[2].Get<uint32>();
                item.stackable = fields[3].Get<uint32>();
                item.quality = fields[4].Get<uint32>();
                item.marketPrice = fields[5].Get<uint32>();
                item.maxCount = fields[6].Get<uint32>();
                item.itemClass = fields[7].Get<uint32>();
                items.push_back(item);
            } while (result->NextRow());
        }

        return items;
    }();

    // Per-class buckets over the shared cache, built once. Index = item class
    // id (core ITEM_CLASS_* range 0..16); out-of-range classes are dropped.
    static std::array<std::vector<const CachedItem*>, 17> classBuckets = [](){
        std::array<std::vector<const CachedItem*>, 17> buckets;
        for (const auto& it : cachedItems)
        {
            if (it.itemClass < buckets.size())
                buckets[it.itemClass].push_back(&it);
        }
        return buckets;
    }();


    std::string countQuery = R"(
        SELECT ii.itemEntry, COUNT(*) as itemCount
        FROM {}.item_instance ii
        INNER JOIN {}.auctionhouse ah ON ii.guid = ah.itemguid
        WHERE ah.houseId = {}
        GROUP BY ii.itemEntry
    )";

    QueryResult countResult = CharacterDatabase.Query(countQuery, characterDbName, characterDbName, houseId);

    std::unordered_map<uint32, uint32> currentCounts;
    if (countResult)
    {
        do
        {
            Field* fields = countResult->Fetch();
            currentCounts[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (countResult->NextRow());
    }

    uint32 count = 0;

    if (!nator->config->sellerConfig.targetSharesEnabled)
    {
        // ---- Legacy path: uniform shuffle over all eligible items ----
        std::vector<CachedItem> shuffled = cachedItems;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::shuffle(shuffled.begin(), shuffled.end(), gen);

        for (const auto& item : shuffled)
        {
            uint32 currentCount = currentCounts[item.entry];
            if (currentCount >= item.maxCount) continue;

            PlaceAuction(item, currentCounts, houseId);
            count++;
            if (count == maxCount) break;
        }
    }
    else
    {
        // ---- Target-share path: weighted class -> uniform entry ----
        count = SellByTargetShare(classBuckets, currentCounts, maxCount, houseId);
    }

    logInfo("Items added houseId("
        + std::to_string(houseId)
        + ") this run: " + std::to_string(count));

};

void AuctionatorSeller::PlaceAuction(
    const CachedItem& item,
    std::unordered_map<uint32, uint32>& currentCounts,
    uint32 houseId)
{
    std::string itemName = item.name;

    uint32 stackSize = item.stackable;
    if (stackSize > 20) {
        stackSize = 20;
    }

    if (stackSize > 1 && nator->config->sellerConfig.randomizeStackSize) {
        stackSize = GetRandomNumber(1, stackSize);
        logDebug("Stack size: " + std::to_string(stackSize));
    }

    float qualityMultiplier =
        Auctionator::GetQualityMultiplier(nator->config->sellerMultipliers, item.quality);

    uint32 price = item.marketPrice > 0 ? item.marketPrice : item.basePrice;
    if (item.marketPrice > 0) {
        logDebug("Using Market over Template [" + itemName + "] " +
            std::to_string(item.marketPrice) + " <--> " + std::to_string(item.basePrice));
    }

    if (price == 0) {
        price = 10000000 * qualityMultiplier;
    }

    uint32 bidPrice = price;
    float bidStartModifier = nator->config->sellerConfig.bidStartModifier;
    bidPrice = GetRandomNumber(bidPrice - (bidPrice * bidStartModifier), bidPrice);

    AuctionatorItem newItem = AuctionatorItem();
    newItem.itemId = item.entry;
    newItem.quantity = 1;
    newItem.houseId = houseId;
    newItem.buyout = uint32(price * stackSize * qualityMultiplier);
    newItem.bid = uint32(bidPrice * stackSize * qualityMultiplier);
    newItem.time = 60 * 60 * 12;
    newItem.stackSize = stackSize;

    logDebug("Adding item: " + itemName
        + " at price of " + std::to_string(newItem.buyout)
        + " to house " + std::to_string(houseId));

    nator->CreateAuction(newItem);
    currentCounts[item.entry]++;
}

uint32 AuctionatorSeller::SellByTargetShare(
    const std::array<std::vector<const CachedItem*>, 17>& classBuckets,
    std::unordered_map<uint32, uint32>& currentCounts,
    uint32 maxCount, uint32 houseId)
{
    auto const& weights = nator->config->sellerConfig.classWeight;

    // Active classes: weight > 0 AND a non-empty bucket. Parallel vectors:
    // class id, its weight, and a "still has capacity" live flag.
    std::vector<uint32> activeClass;
    std::vector<float> activeWeight;
    for (uint32 c = 0; c < classBuckets.size(); ++c)
    {
        if (weights[c] > 0.0f && !classBuckets[c].empty())
        {
            activeClass.push_back(c);
            activeWeight.push_back(weights[c]);
        }
    }

    std::random_device rd;
    std::mt19937 gen(rd());

    uint32 count = 0;
    std::array<uint32, 17> placedByClass = {};

    // Loop until budget spent or no active class can place.
    while (count < maxCount && !activeClass.empty())
    {
        // Weighted pick of an active class index.
        float total = 0.0f;
        for (float w : activeWeight) total += w;
        std::uniform_real_distribution<float> pick(0.0f, total);
        float roll = pick(gen);
        size_t idx = 0;
        for (; idx < activeWeight.size(); ++idx)
        {
            roll -= activeWeight[idx];
            if (roll <= 0.0f) break;
        }
        if (idx >= activeClass.size()) idx = activeClass.size() - 1;

        uint32 classId = activeClass[idx];
        const auto& bucket = classBuckets[classId];

        // Try a bounded number of random entries in this class before giving up.
        std::uniform_int_distribution<size_t> entryDist(0, bucket.size() - 1);
        bool placed = false;
        uint32 tries = std::min<uint32>(bucket.size(), 32);
        for (uint32 t = 0; t < tries; ++t)
        {
            const CachedItem* item = bucket[entryDist(gen)];
            if (currentCounts[item->entry] < item->maxCount)
            {
                PlaceAuction(*item, currentCounts, houseId);
                count++;
                placedByClass[classId]++;
                placed = true;
                break;
            }
        }

        if (!placed)
        {
            // Confirm the class is genuinely saturated (full scan) before dropping;
            // a random-miss streak alone shouldn't evict a class with capacity left.
            bool hasCapacity = false;
            for (const CachedItem* item : bucket)
            {
                if (currentCounts[item->entry] < item->maxCount) { hasCapacity = true; break; }
            }
            if (!hasCapacity)
            {
                activeClass.erase(activeClass.begin() + idx);
                activeWeight.erase(activeWeight.begin() + idx);
            }
        }
    }

    // Per-class breakdown for live tuning.
    std::string breakdown;
    for (uint32 c = 0; c < placedByClass.size(); ++c)
        if (placedByClass[c] > 0)
            breakdown += " c" + std::to_string(c) + "=" + std::to_string(placedByClass[c]);
    logDebug("Target-share placements house(" + std::to_string(houseId) + "):" + breakdown);

    return count;
}

uint32 AuctionatorSeller::GetRandomNumber(uint32 min, uint32 max)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(min, max);
    return dis(gen);
}
