cmake_minimum_required(VERSION 3.20)
project(war3d LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 依赖放在本项目 third_party 内（GLFW 3.4 WIN64 / glad43 / ImGui），不再依赖外部工程
set(TP "${CMAKE_CURRENT_LIST_DIR}/third_party")
set(GLFW_ROOT "${TP}/glfw-3.4.bin.WIN64")
# ImGui 放在本项目内，避免改动 fitsplat_git
set(IMGUI "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui")

if(NOT EXISTS "${GLFW_ROOT}/lib-vc2022/glfw3.lib")
    message(FATAL_ERROR "找不到 GLFW：${GLFW_ROOT}/lib-vc2022/glfw3.lib")
endif()
if(NOT EXISTS "${TP}/glad.c")
    message(FATAL_ERROR "找不到 glad：${TP}/glad.c")
endif()
if(NOT EXISTS "${IMGUI}/imgui.cpp")
    message(FATAL_ERROR "找不到 ImGui：${IMGUI}/imgui.cpp")
endif()

add_executable(war3d
    src/main.cpp
    src/terrain.cpp
    src/events.cpp
    src/text_atlas.cpp
    src/routes.cpp
    src/water.cpp
    src/river_water.cpp
    src/map_symbols.cpp
    src/narration.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_glfw.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp
    ${TP}/glad.c
)

target_include_directories(war3d PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${TP}/include
    ${GLFW_ROOT}/include
    ${IMGUI}
    ${IMGUI}/backends
)

target_link_libraries(war3d PRIVATE
    ${GLFW_ROOT}/lib-vc2022/glfw3.lib
    opengl32
)

if(MSVC)
    target_compile_options(war3d PRIVATE /W4 /utf-8)
    # 显式指定系统库路径（链接器环境的 LIB 变量可能被过滤，
    # 裸库名 opengl32/kernel32/msvcprt 等会因找不到 .lib 而链接失败）
    get_filename_component(_MSVC_ROOT "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_MSVC_ROOT "${_MSVC_ROOT}/../../.." ABSOLUTE)   # .../MSVC/<ver>
    target_link_options(war3d PRIVATE "/LIBPATH:${_MSVC_ROOT}/lib/x64")
    file(GLOB _WIN_KITS "C:/Program Files (x86)/Windows Kits/10/Lib/*")
    foreach(_kit ${_WIN_KITS})
        if(EXISTS "${_kit}/um/x64")
            target_link_options(war3d PRIVATE
                "/LIBPATH:${_kit}/um/x64"
                "/LIBPATH:${_kit}/ucrt/x64")
        endif()
    endforeach()
endif()
