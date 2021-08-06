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

extern "C" {
typedef struct {
    uint32_t val;
    size_t weight;
} wre_test_struct_t;

static bool is_lower_int(const void* i1, const void* i2) {
    return ((wre_test_struct_t*)i1)->val < ((wre_test_struct_t*)i1)->val;
}
}

TEST_CASE("Weight-Ratio-Extended tree test") {
    const size_t TAB_SIZE=100u;
    wre_test_struct_t blocks[TAB_SIZE];
    for (int i=0; i<TAB_SIZE; ++i) {
        blocks[i].val=i;
        blocks[i].weight=TAB_SIZE-i; // TODO handle corner case: 0
    }

    wre_tree_t *tree;
    wre_create(&tree, is_lower_int);

    SUBCASE("Simple adds") {
        wre_put(tree, &blocks[6], blocks[6].weight); // value 6, weight: 94
        CHECK_EQ(tree->rootNode->subtreeWeight, 94u);
        CHECK_EQ(tree->rootNode->subtreeNodesNum, 0u);
        CHECK_EQ(tree->rootNode->left, nullptr);
        CHECK_EQ(tree->rootNode->right, nullptr);
        CHECK_EQ(tree->rootNode->parent, nullptr);
        CHECK_EQ(tree->rootNode->which, ROOT_NODE);
        CHECK_EQ(tree->rootNode->data, &blocks[6]);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->val, 6);
        CHECK_EQ(((wre_test_struct_t*)tree->rootNode->data)->weight, 94);
        wre_put(tree, &blocks[3], blocks[3].weight); // value 3, weight: 97
        CHECK_EQ(tree->rootNode->subtreeWeight, 191u);
        CHECK_EQ(tree->rootNode->subtreeNodesNum, 1u);
        CHECK_EQ(tree->rootNode->left->subtreeWeight, 97);
        CHECK_EQ(tree->rootNode->left->subtreeNodesNum, 0);
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

    SUBCASE("Add multiple nodes") {
        size_t accumulated_weight=0u;
        // add all nodes in regular order
        for (int i=0; i<TAB_SIZE; ++i) {
            wre_put(tree, &blocks[i], blocks[i].weight);
            accumulated_weight += blocks[i].weight;
            CHECK_EQ(tree->rootNode->subtreeWeight, accumulated_weight);
        }
        // check contents
        CHECK_EQ(tree->rootNode->subtreeWeight, 5050u);
        // TODO more checks!
    }
    // TODO check scenario - two structures, same hotness!!!
    // the case should still be correctly handled!!!
    wre_destroy(tree);
}
