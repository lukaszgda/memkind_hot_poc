#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
extern "C" {
#include "memkind/internal/ranking.h"
#include "memkind/internal/wre_avl_tree.h"
}

TEST_CASE("Ranking test") {
    ranking_t *ranking;
    ranking_create(&ranking);

    const size_t BLOCKS_SIZE=100u;
    struct ttype blocks[BLOCKS_SIZE];
    for (int i=0; i<BLOCKS_SIZE; ++i) {
        blocks[i].size=BLOCKS_SIZE-i;
        blocks[i].n2=i;
        ranking_add(ranking, &blocks[i]);
    }
    // initialized
    SUBCASE("check hotness highest") {
        double RATIO_PMEM_ONLY=0;
        double thresh_highest =
            ranking_calculate_hot_threshold_dram_total(
                ranking, RATIO_PMEM_ONLY);
        double thresh_highest_pmem =
            ranking_calculate_hot_threshold_dram_pmem(ranking, 0);
        CHECK_EQ(thresh_highest, thresh_highest_pmem); // double for equality
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
            ranking_calculate_hot_threshold_dram_total(
                ranking, RATIO_DRAM_ONLY);
        double thresh_lowest_pmem =
            ranking_calculate_hot_threshold_dram_pmem(
                ranking, std::numeric_limits<double>::max());
        CHECK_EQ(thresh_lowest, thresh_lowest_pmem); // double for equality
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
            ranking_calculate_hot_threshold_dram_total(ranking, RATIO_EQUAL);
        double thresh_equal_pmem =
            ranking_calculate_hot_threshold_dram_pmem(ranking, 1);
        CHECK_EQ(thresh_equal, thresh_equal_pmem);
        CHECK_EQ(thresh_equal, 29);
        for (int i=0; i<29; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=29; i<100; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK_EQ(BLOCKS_SIZE, 100);
    }
    SUBCASE("check hotness 50:50, removed") {
        const size_t SUBSIZE=10u;
        for (int i=SUBSIZE; i<BLOCKS_SIZE; ++i) {
            ranking_remove(ranking, &blocks[i]);
        }
        double RATIO_EQUAL_TOTAL=0.5;
        double RATIO_EQUAL_PMEM=1;
        double thresh_equal = ranking_calculate_hot_threshold_dram_total(
            ranking, RATIO_EQUAL_TOTAL);
        double thresh_equal_pmem = ranking_calculate_hot_threshold_dram_pmem(
            ranking, RATIO_EQUAL_PMEM);
        // hand calculations:
        // 100, 99, 98, 97, 96, 95, 94, 93, 92, 91
        // sum:
        // 100, 199, 297, 394, 490 <- this is the one we are looking for
        CHECK_EQ(thresh_equal, 4);
        CHECK_EQ(thresh_equal, thresh_equal_pmem);
        for (int i=0; i<4; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=4; i<10; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK_EQ(BLOCKS_SIZE, 100);
        CHECK_EQ(SUBSIZE, 10);
    }

    ranking_destroy(ranking);
}

TEST_CASE("Ranking test same hotness") {
    ranking_t *ranking;
    ranking_create(&ranking);

    const size_t BLOCKS_SIZE=100u;
    struct ttype blocks[BLOCKS_SIZE];
    for (int i=0; i<BLOCKS_SIZE; ++i) {
        blocks[i].size=BLOCKS_SIZE-i;
        blocks[i].n2=i%50;
        ranking_add(ranking, &blocks[i]);
    }
    // initialized
    SUBCASE("check hotness highest") {
        double RATIO_PMEM_ONLY_TOTAL=0;
        double RATIO_PMEM_ONLY_PMEM=0;
        double thresh_highest = ranking_calculate_hot_threshold_dram_total(
            ranking, RATIO_PMEM_ONLY_TOTAL);
        double thresh_highest_pmem = ranking_calculate_hot_threshold_dram_pmem(
            ranking, RATIO_PMEM_ONLY_PMEM);
        CHECK_EQ(thresh_highest, (BLOCKS_SIZE-1)%50);
        CHECK_EQ(thresh_highest, 49);
        CHECK_EQ(thresh_highest, thresh_highest_pmem);
        for (int i=0; i<BLOCKS_SIZE-1; ++i) {
            CHECK_EQ(ranking_is_hot(ranking, &blocks[i]), i==49);
        }
        CHECK(ranking_is_hot(ranking, &blocks[BLOCKS_SIZE-1]));
    }
    SUBCASE("check hotness lowest") {
        double RATIO_DRAM_ONLY=1;
        double thresh_lowest = ranking_calculate_hot_threshold_dram_total(
            ranking, RATIO_DRAM_ONLY);
        double thresh_lowest_pmem = ranking_calculate_hot_threshold_dram_pmem(
            ranking, std::numeric_limits<double>::max());
        CHECK_EQ(thresh_lowest, 0);
        CHECK_EQ(thresh_lowest, thresh_lowest_pmem);
        for (int i=0; i<BLOCKS_SIZE; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
    }
    SUBCASE("check hotness 50:50") {
        double RATIO_EQUAL_TOTAL=0.5;
        double RATIO_EQUAL_DRAM=1;
        // when grouped in pairs, we get 150, 148, .., 52
        // arithmetic series a0 = 150, r=-2, n=50
        // we now want to find n_50: s_n_50 = s_n/2
        // sn == 5050 => sn/2 == 2525
        // the equation to solve for n_50:
        // 2525 == n_50*(150-2*(n_50-1))/2
        // 5050 = 150*n_50-2*n_50^2+2*n_50 = 152*n_50-2*n_50^2
        // n_50^2-76*n_50+2525 = 0
        // delta = 76^2-4*2525 = 5776-10000
        double thresh_equal = ranking_calculate_hot_threshold_dram_total(
            ranking, RATIO_EQUAL_TOTAL);
        CHECK_EQ(thresh_equal, 19);
        for (int i=0; i<19; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=19; i<50; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=50; i<69; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=69; i<100; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK_EQ(BLOCKS_SIZE, 100);
    }
    SUBCASE("check hotness 50:50, removed") {
        const size_t SUBSIZE=10u;
        for (int i=SUBSIZE; i<BLOCKS_SIZE; ++i) {
            ranking_remove(ranking, &blocks[i]);
        }
        double RATIO_EQUAL_TOTAL=0.5;
        double RATIO_EQUAL_PMEM=1;
        double thresh_equal = ranking_calculate_hot_threshold_dram_total(
            ranking, RATIO_EQUAL_TOTAL);
        double thresh_equal_pmem = ranking_calculate_hot_threshold_dram_pmem(
            ranking, RATIO_EQUAL_PMEM);
        // hand calculations:
        // 100, 99, 98, 97, 96, 95, 94, 93, 92, 91
        // sum:
        // 100, 199, 297, 394, 490 <- this is the one we are looking for
        CHECK_EQ(thresh_equal, 4);
        CHECK_EQ(thresh_equal, thresh_equal_pmem);
        for (int i=0; i<4; ++i) {
            CHECK(!ranking_is_hot(ranking, &blocks[i]));
        }
        for (int i=4; i<10; ++i) {
            CHECK(ranking_is_hot(ranking, &blocks[i]));
        }
        CHECK_EQ(BLOCKS_SIZE, 100);
        CHECK_EQ(SUBSIZE, 10);
    }
    ranking_destroy(ranking);
}

extern "C" {
typedef struct {
    uint32_t val;
    size_t weight;
} wre_test_struct_t;

static bool is_lower_int(const void* i1, const void* i2) {
    return ((wre_test_struct_t*)i1)->val < ((wre_test_struct_t*)i2)->val;
}
}

TEST_CASE("Weight-Ratio-Extended tree test") {
    const size_t TAB_SIZE=100u;
    const size_t EXTENDED_TAB_SIZE=200u;
    wre_test_struct_t blocks[EXTENDED_TAB_SIZE];
    for (int i=0; i<EXTENDED_TAB_SIZE; ++i) {
        blocks[i].val=i;
        blocks[i].weight=abs(((int64_t)TAB_SIZE)-i);
    }

    wre_tree_t *tree;
    wre_create(&tree, is_lower_int);



    SUBCASE("Simple adds") {
        wre_put(tree, &blocks[6], blocks[6].weight); // value 6, weight: 94
        CHECK_EQ(tree->rootNode->subtreeWeight, 94u);
        CHECK_EQ(tree->rootNode->height, 0u);
        CHECK_EQ(tree->rootNode->left, nullptr);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
        wre_put(tree, &blocks[3], blocks[3].weight); // value 3, weight: 97
        CHECK_EQ(tree->rootNode->subtreeWeight, 191u);
        CHECK_EQ(tree->rootNode->height, 1u);
        CHECK_EQ(tree->rootNode->left->subtreeWeight, 97);
        CHECK_EQ(tree->rootNode->left->height, 0);
        CHECK_EQ(tree->rootNode->left->left, nullptr);
        CHECK_EQ(tree->rootNode->left->right, nullptr);
        CHECK_EQ(tree->rootNode->left->parent, tree->rootNode);
        CHECK_EQ(tree->rootNode->left->which, LEFT_NODE);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
    }

    SUBCASE("Simple adds-removes") {
        wre_put(tree, &blocks[6], blocks[6].weight); // value 6, weight: 94
        CHECK_EQ(tree->rootNode->subtreeWeight, 94u);
        CHECK_EQ(tree->rootNode->height, 0u);
        CHECK_EQ(tree->rootNode->left, nullptr);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
        void* data = wre_remove(tree, &blocks[6]);
        CHECK_EQ(data, &blocks[6]);
        CHECK_EQ(tree->rootNode, nullptr);
        wre_put(tree, &blocks[6], blocks[6].weight); // value 6, weight: 94
        wre_put(tree, &blocks[3], blocks[3].weight); // value 3, weight: 97
        CHECK_EQ(tree->rootNode->subtreeWeight, 191u);
        CHECK_EQ(tree->rootNode->height, 1u);
        CHECK_EQ(tree->rootNode->left->subtreeWeight, 97);
        CHECK_EQ(tree->rootNode->left->height, 0);
        CHECK_EQ(tree->rootNode->left->left, nullptr);
        CHECK_EQ(tree->rootNode->left->right, nullptr);
        CHECK_EQ(tree->rootNode->left->parent, tree->rootNode);
        CHECK_EQ(tree->rootNode->left->which, LEFT_NODE);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
        data = wre_remove(tree, &blocks[3]);
        CHECK_EQ(data, &blocks[3]);
        CHECK_EQ(tree->rootNode->subtreeWeight, 94u);
        CHECK_EQ(tree->rootNode->height, 0u);
        CHECK_EQ(tree->rootNode->left, nullptr);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
        wre_put(tree, &blocks[3], blocks[3].weight); // value 3, weight: 97
        data = wre_remove(tree, &blocks[6]);
        CHECK_EQ(data, &blocks[6]);
        CHECK_EQ(tree->rootNode->subtreeWeight, 97u);
        CHECK_EQ(tree->rootNode->height, 0u);
        CHECK_EQ(tree->rootNode->left, nullptr);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[3]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 3);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 97);
    }

    SUBCASE("Add multiple nodes") {
        size_t accumulated_weight=0u;
        // add all nodes in regular order
        for (int i=0; i<TAB_SIZE; ++i) {
            wre_put(tree, &blocks[i], blocks[i].weight);
            accumulated_weight += blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        CHECK_EQ(tree->rootNode->height, 6);
        // check contents
        CHECK_EQ(tree->rootNode->subtreeWeight, 5050u);
        // TODO more checks!
    }

    SUBCASE("Add-remove multiple nodes") {
        size_t accumulated_weight=0u;
        // add all nodes in regular order
        for (int i=0; i<EXTENDED_TAB_SIZE; ++i) {
            wre_put(tree, &blocks[i], blocks[i].weight);
            accumulated_weight += blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        CHECK_EQ(tree->rootNode->height, 7);
        for (int i=TAB_SIZE; i<EXTENDED_TAB_SIZE; ++i) {
            void* removed = wre_remove(tree, &blocks[i]);
            CHECK_EQ(removed, &blocks[i]);
            accumulated_weight -= blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        CHECK_EQ(tree->rootNode->height, 6);
        // check contents
        CHECK_EQ(tree->rootNode->subtreeWeight, 5050u);
    }

    SUBCASE("Add-remove multiple nodes, descending order") {
        size_t accumulated_weight=0u;
        // add all nodes in regular order
        for (int i=0; i<EXTENDED_TAB_SIZE; ++i) {
            wre_put(tree, &blocks[i], blocks[i].weight);
            accumulated_weight += blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        CHECK_EQ(tree->rootNode->height, 7);
        for (int i=EXTENDED_TAB_SIZE-1; i>=TAB_SIZE; --i) {
            void* removed = wre_remove(tree, &blocks[i]);
            CHECK_EQ(removed, &blocks[i]);
            accumulated_weight -= blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        CHECK_EQ(tree->rootNode->height, 6);
        // check contents
        CHECK_EQ(tree->rootNode->subtreeWeight, 5050u);
    }
    // the case should still be correctly handled!!!
    wre_destroy(tree);
}
