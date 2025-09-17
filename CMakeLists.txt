cmake_minimum_required(VERSION 3.16)

project(Plin)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_FLAGS "-g -march=native") 
# set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -g")
# set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fsanitize=address")

# link_directories(/usr/lib/gcc/x86_64-linux-gnu/10/)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcuckoo/include) #must include this before add_executable
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libcuckoo)
add_executable(c client.cpp)
add_executable(s server.cpp)


# add_compile_options(-fsanitize=thread)

# add_link_options(-fsanitize=thread)


target_link_libraries(c -pthread)
target_link_libraries(s -pthread)



set(CMAKE_BUILD_TYPE Debug)



