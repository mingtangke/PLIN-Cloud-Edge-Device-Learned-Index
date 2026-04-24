# cloud: single cloud server with global LSTM and cross-edge routing (stub).
set(CLOUD_SOURCES
    cloud_server.cpp
)
if(PLIN_HAVE_TORCH)
    list(APPEND CLOUD_SOURCES cloud_lstm_runner.cpp)
else()
    list(APPEND CLOUD_SOURCES cloud_lstm_runner_stub.cpp)
endif()

add_executable(cloud_server ${CLOUD_SOURCES})
target_include_directories(cloud_server PRIVATE
    ${PLIN_SRC_INCLUDE}
)
target_link_libraries(cloud_server PRIVATE plin_common pthread)

if(PLIN_HAVE_TORCH)
    target_link_libraries(cloud_server PRIVATE "${TORCH_LIBRARIES}")
    target_compile_definitions(cloud_server PRIVATE PLIN_HAVE_TORCH=1)
endif()
