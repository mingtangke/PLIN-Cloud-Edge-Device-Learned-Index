# common: protocol, RPC framing, topology/range map.
add_library(plin_common STATIC
    rpc.cpp
    transport.cpp
    range_map.cpp
)
if(PLIN_HAVE_RDMA)
    target_sources(plin_common PRIVATE rdma_transport.cpp)
    target_include_directories(plin_common PUBLIC
        ${PLIN_IBVERBS_INCLUDE_DIR}
        ${PLIN_RDMACM_INCLUDE_DIR}
    )
    target_compile_definitions(plin_common PUBLIC PLIN_ENABLE_RDMA=1)
    target_link_libraries(plin_common PUBLIC
        ${PLIN_IBVERBS_LIBRARY}
        ${PLIN_RDMACM_LIBRARY}
    )
endif()
target_include_directories(plin_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PLIN_SRC_INCLUDE}          # common/proto.h style includes
)
target_link_libraries(plin_common PUBLIC pthread)

# M2 loopback test (not installed; build and run locally)
add_executable(loopback_test loopback_test.cpp)
target_link_libraries(loopback_test PRIVATE plin_common)
