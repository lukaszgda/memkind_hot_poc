cmake_minimum_required (VERSION 2.8.11)
project (ranking_test)

include_directories("${CMAKE_SOURCE_DIR}/../../include")

add_executable (ranking_test
    main.cpp
    "${CMAKE_SOURCE_DIR}/../../src/ranking.cpp"
)
