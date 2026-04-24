# edge: adaptive PLIN server (bulk_load + EDGE_FETCH_REQ + PLIN_PARAM_PUSH).
add_executable(edge_server edge_server.cpp)
target_include_directories(edge_server PRIVATE
    ${PLIN_SRC_INCLUDE}          # common/proto.h style includes
    ${PLIN_CORE_INCLUDE}         # PLIN index core
    ${PLIN_LIBCUCKOO_INCLUDE}
)
target_compile_options(edge_server PRIVATE -O2)
target_link_libraries(edge_server PRIVATE plin_common pthread)
