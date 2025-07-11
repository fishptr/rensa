cmake_minimum_required(VERSION 3.15)
project(rensa)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src")
    message(FATAL_ERROR "src directory not found!")
endif()

set(SOURCE_FILES
    src/dllmain.cpp
    src/hooks/d3d12hook.cpp
    
    # Kiero
    vendor/kiero/kiero.cpp

    # ImGui files
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_demo.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_dx12.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
)

foreach(SOURCE_FILE ${SOURCE_FILES})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_FILE}")
        message(FATAL_ERROR "Source file not found: ${SOURCE_FILE}")
    endif()
endforeach()

include_directories(
    src
    vendor
    vendor/minhook/include
    vendor/kiero
    vendor/imgui
    vendor/imgui/backends
)

add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES})

add_subdirectory(vendor/minhook)

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    d3d12
    dxgi
)

if(MSVC)
    add_compile_options(/std:c++17)
endif()