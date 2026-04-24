cmake_minimum_required(VERSION 3.16)
project(PlinCloudEdgeDevice CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -march=native -Wall -Wno-unused-parameter")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- third_party -----------------------------------------------------------
# libcuckoo: header-only, kept at repo root
set(PLIN_LIBCUCKOO_INCLUDE "${CMAKE_SOURCE_DIR}/libcuckoo" CACHE PATH "")

# TLX BTree: header-only interface library
add_library(tlx_btree INTERFACE)
target_include_directories(tlx_btree INTERFACE "${CMAKE_SOURCE_DIR}/third_party/tlx")

# libtorch: check third_party/ first, then miniconda PyTorch, then skip.
set(PLIN_LIBTORCH_DIR "${CMAKE_SOURCE_DIR}/third_party/libtorch")
set(PLIN_MINICONDA_TORCH "/root/miniconda3/lib/python3.12/site-packages/torch")
if(EXISTS "${PLIN_LIBTORCH_DIR}/share/cmake/Torch/TorchConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${PLIN_LIBTORCH_DIR}")
    find_package(Torch REQUIRED)
    set(PLIN_HAVE_TORCH ON)
    message(STATUS "libtorch found at ${PLIN_LIBTORCH_DIR}")
elseif(EXISTS "${PLIN_MINICONDA_TORCH}/share/cmake/Torch/TorchConfig.cmake")
    # PyTorch CUDA wheels may need nvcc during TorchConfig.cmake even when the
    # project itself has no CUDA sources. AutoDL images usually keep it here but
    # do not always put it on PATH.
    if(NOT DEFINED CMAKE_CUDA_COMPILER AND EXISTS "/usr/local/cuda/bin/nvcc")
        set(CMAKE_CUDA_COMPILER "/usr/local/cuda/bin/nvcc" CACHE FILEPATH "" FORCE)
    endif()
    # Prefer CPU-only inference settings when Torch honors them; CUDA wheels may
    # still enable CUDA internally and use CMAKE_CUDA_COMPILER above.
    set(CAFFE2_USE_CUDA    OFF CACHE BOOL "" FORCE)
    set(CAFFE2_USE_CUDNN   OFF CACHE BOOL "" FORCE)
    set(USE_CUDA           OFF CACHE BOOL "" FORCE)
    list(APPEND CMAKE_PREFIX_PATH "${PLIN_MINICONDA_TORCH}")
    find_package(Torch REQUIRED)
    set(PLIN_HAVE_TORCH ON)
    message(STATUS "libtorch found via miniconda PyTorch at ${PLIN_MINICONDA_TORCH}")
else()
    set(PLIN_HAVE_TORCH OFF)
    message(WARNING "libtorch NOT found. Cloud/End LSTM runners will be stubbed. "
                    "Run scripts/fetch_libtorch.sh or install PyTorch to enable.")
endif()

# --- project layers --------------------------------------------------------
set(PLIN_SRC_INCLUDE "${CMAKE_SOURCE_DIR}/src")
set(PLIN_CORE_INCLUDE "${CMAKE_SOURCE_DIR}/src/core/index")

add_subdirectory(src/common common)
add_subdirectory(src/edge edge)
add_subdirectory(src/end end)
add_subdirectory(src/cloud cloud)
