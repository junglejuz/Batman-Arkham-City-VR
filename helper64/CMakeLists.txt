# Batman City DLSS helper - a separate 64-bit build (the game and batman_city.dll are 32-bit).
#   cmake -S helper64 -B build64 -A x64 && cmake --build build64 --config RelWithDebInfo
cmake_minimum_required(VERSION 3.20)
project(bc_dlss64 CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "bc_dlss64 must be built 64-bit (-A x64)")
endif()
# NVIDIA NGX SDK 310.9.1 (from Luma), shared with AKN.
set(NGX "${CMAKE_CURRENT_SOURCE_DIR}/../../akvr_native/third_party/ngx")
add_executable(bc_dlss64 WIN32 bc_dlss64.cpp dlss_shared.h)
target_include_directories(bc_dlss64 PRIVATE "${NGX}/include")
target_link_libraries(bc_dlss64 PRIVATE d3d11 dxgi "${NGX}/lib/nvsdk_ngx_d.lib")
target_compile_definitions(bc_dlss64 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
