# end: device-side node (local B+, parent PLIN cache, hot cache, edge connection).
set(END_SOURCES
    end_node.cpp
    # hot_cache.cpp and parent_plin_cache.cpp are header-only; their headers are
    # included only by end_node.cpp to avoid ODR violations with b_plus.h/serialize.h.
)
if(PLIN_HAVE_TORCH)
    list(APPEND END_SOURCES end_lstm_runner.cpp)
else()
    list(APPEND END_SOURCES end_lstm_runner_stub.cpp)
endif()

add_executable(end_node ${END_SOURCES})
target_include_directories(end_node PRIVATE
    ${PLIN_SRC_INCLUDE}          # common/proto.h style includes
    ${PLIN_CORE_INCLUDE}         # cache_model.h, serialize.h, parameters.h
    ${PLIN_LIBCUCKOO_INCLUDE}
)
target_compile_options(end_node PRIVATE -O2)
target_link_libraries(end_node PRIVATE
    plin_common
    tlx_btree
    pthread
)

if(PLIN_HAVE_TORCH)
    target_link_libraries(end_node PRIVATE "${TORCH_LIBRARIES}")
    target_compile_definitions(end_node PRIVATE PLIN_HAVE_TORCH=1)
endif()
