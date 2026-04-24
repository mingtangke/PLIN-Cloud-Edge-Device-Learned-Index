# common: protocol, RPC framing, topology/range map.
add_library(plin_common STATIC
    rpc.cpp
    range_map.cpp
)
target_include_directories(plin_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PLIN_SRC_INCLUDE}          # common/proto.h style includes
)
target_link_libraries(plin_common PUBLIC pthread)

# M2 loopback test (not installed; build and run locally)
add_executable(loopback_test loopback_test.cpp)
target_link_libraries(loopback_test PRIVATE plin_common)
