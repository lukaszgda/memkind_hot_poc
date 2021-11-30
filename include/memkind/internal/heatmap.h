#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heatmap_aggregator heatmap_aggregator_t;

typedef struct HeatmapEntry {
    double dram_to_total;
    double hotness;
} HeatmapEntry_t;

heatmap_aggregator_t *heatmap_aggregator_create();
void heatmap_aggregator_destroy(struct heatmap_aggregator *aggregator);
void heatmap_aggregator_aggregate(struct heatmap_aggregator *aggregator,
                                  struct HeatmapEntry *entry);
/// @warning please use **heatmap_free_info** to free returned memory
char *heatmap_dump_info(struct heatmap_aggregator *aggregator);
void heatmap_free_info(char *info);

#ifdef __cplusplus
}
#endif
