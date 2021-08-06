cmake_minimum_required (VERSION 3.0)
project (ranking_test)

include_directories("${CMAKE_SOURCE_DIR}/../../include")

add_executable (ranking_test
    main.cpp
    "${CMAKE_SOURCE_DIR}/../../src/ranking.cpp"
    "${CMAKE_SOURCE_DIR}/../../src/wre_avl_tree.c"
)
