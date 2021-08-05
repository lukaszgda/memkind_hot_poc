#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
extern "C" {
#include "memkind/internal/ranking.h"
}

TEST_CASE("Simple test") {
    //
    ranking_t *ranking;
    ranking_create(&ranking);

    const size_t BLOCKS_SIZE=100u;
    struct tblock blocks[BLOCKS_SIZE];
    for (int i=0; i<BLOCKS_SIZE; ++i) {
        blocks[i].size=BLOCKS_SIZE-i;
        blocks[i].n2=i;
        ranking_add(ranking, &blocks[i]);
    }
    // initialized
    SUBCASE("check hotness highest") {
        double RATIO_PMEM_ONLY=0;
        double thresh_highest =
            ranking_calculate_hot_threshold(ranking, RATIO_PMEM_ONLY);
        CHECK_EQ(thresh_highest, BLOCKS_SIZE-1);
        CHECK_EQ(thresh_highest, 99);
        for (int i=0; i<BLOCKS_SIZE-1; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK(ranking_is_hot(ranking, &blocks[BLOCKS_SIZE-1]));
    }
    SUBCASE("check hotness lowest") {
        double RATIO_DRAM_ONLY=1;
        double thresh_lowest =
            ranking_calculate_hot_threshold(ranking, RATIO_DRAM_ONLY);
        CHECK_EQ(thresh_lowest, 0);
        for (int i=0; i<BLOCKS_SIZE; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
    }
    SUBCASE("check hotness 50:50") {
        double RATIO_EQUAL=0.5;
        // equal by size
        // total size allocated:
        // (1.+BLOCKS_SIZE)/2*BLOCKS_SIZE (arithmetic series)
        // half the size:
        // (1.+BLOCKS_SIZE)/4*BLOCKS_SIZE = sn
        double HALF_SIZE=(1.+BLOCKS_SIZE)/4*BLOCKS_SIZE;
        // how many previous elements have half the size:
        // (1.+n)/2*n=sn => 1+n=2*sn/n => n^2+n-2*sn=0 => delta=1+8*sn
        double delta = 1+8*HALF_SIZE;
        size_t n = floor((-1+sqrt(delta))/2);
        CHECK_EQ(n, 70); // calculated by hand
        double thresh_equal =
            ranking_calculate_hot_threshold(ranking, RATIO_EQUAL);
        CHECK_EQ(thresh_equal, 29);
        for (int i=0; i<29; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=30; i<100; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK_EQ(BLOCKS_SIZE, 100);
    }

    ranking_destroy(ranking);
}
