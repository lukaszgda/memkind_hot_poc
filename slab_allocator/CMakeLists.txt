cmake_minimum_required(VERSION 3.10)

project(slab_allocator)

add_executable(${PROJECT_NAME} slab_allocator.c bigary.c)
