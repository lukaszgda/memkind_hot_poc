#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cassert>

#include "memkind/internal/heatmap.h"
#include "memkind/internal/memkind_private.h"

#include "jemalloc/jemalloc.h"

struct HeatmapByteEntry {
    uint8_t dram_to_total;
    uint8_t hotness;
};

static bool compare_hotness_desc(const HeatmapEntry_t &a,
                                 const HeatmapEntry_t &b)
{
    return a.hotness > b.hotness;
}

class HeatmapAggregator
{
    std::vector<HeatmapEntry_t> entries;

    /// @warn clears all entries
    std::vector<HeatmapByteEntry> CalculateNormalizedEntries()
    {
        if (entries.size() == 0u)
            return std::vector<HeatmapByteEntry>();
        for (HeatmapEntry_t &entry : entries) {
            assert(entry.hotness >= 0);
            if (entry.hotness != 0 )
            entry.hotness = log(entry.hotness);
        }
        std::sort(std::begin(entries), std::end(entries), compare_hotness_desc);
        double max_hotness = entries[0].hotness;
        std::vector<HeatmapByteEntry> ret;
        ret.reserve(entries.size());
        for (HeatmapEntry_t &entry : entries) {
            HeatmapByteEntry target;
            target.hotness = (uint8_t)(0xFF * (entry.hotness / max_hotness));
            target.dram_to_total = (uint8_t)(0xFF * entry.dram_to_total);
            ret.push_back(target);
        }

        entries.clear();

        return ret;
    }

public:
    void AddType(const HeatmapEntry_t &entry)
    {
        entries.push_back(entry);
    }
    std::string Serialize()
    {
        auto normalized = CalculateNormalizedEntries();
        std::stringstream serialized;
        serialized << "heatmap_data = [" << std::hex;
        //         serialized << "heatmap_data = [" ;
        for (const HeatmapByteEntry &entry : normalized) {
            serialized << (uint32_t)entry.hotness << ","
                       << (uint32_t)entry.dram_to_total << ";";
        }
        serialized << "]" << std::endl;

        return serialized.str();
    }
};

struct heatmap_aggregator {
    HeatmapAggregator aggregator;
};

MEMKIND_EXPORT heatmap_aggregator_t *heatmap_aggregator_create()
{
    heatmap_aggregator_t *aggregator =
        (heatmap_aggregator_t *)jemk_malloc(sizeof(heatmap_aggregator_t));
    (void)new ((void *)(&aggregator->aggregator)) HeatmapAggregator;
    return aggregator;
}

MEMKIND_EXPORT void
heatmap_aggregator_destroy(struct heatmap_aggregator *aggregator)
{
    aggregator->aggregator.~HeatmapAggregator();
    jemk_free(aggregator);
}

MEMKIND_EXPORT void
heatmap_aggregator_aggregate(struct heatmap_aggregator *aggregator,
                             struct HeatmapEntry *entry)
{
    aggregator->aggregator.AddType(*entry);
}

MEMKIND_EXPORT char *heatmap_dump_info(struct heatmap_aggregator *aggregator)
{
    std::string serialized = aggregator->aggregator.Serialize();
    char *serialized_c = (char *)jemk_malloc(serialized.size() + 1);
    memcpy(serialized_c, serialized.c_str(), serialized.size() + 1);
    return serialized_c;
}

MEMKIND_EXPORT void heatmap_free_info(char *info)
{
    jemk_free(info);
}

// c bindings
