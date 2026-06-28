cmake_minimum_required(VERSION 3.20)
project(simbrief_hub LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# Configurable paths
# ==============================================================================
set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/../SDK" CACHE PATH "X-Plane SDK root")
set(XPLIB "${CMAKE_CURRENT_SOURCE_DIR}/../xplib" CACHE PATH "xplib root")

# ==============================================================================
# Version header generation (mirrors Makefile.common logic)
# ==============================================================================
find_package(Git)
set(SIMBRIEF_HUB_COMMIT "unknown")
set(XPLIB_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --pretty=format:%h
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE SIMBRIEF_HUB_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --pretty=format:%h
        WORKING_DIRECTORY ${XPLIB}
        OUTPUT_VARIABLE XPLIB_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
set(VERSION_SHORT "unknown")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version.mak")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.mak" _ver_mak)
    if(_ver_mak MATCHES "VERSION=([^\r\n]+)")
        set(VERSION_SHORT "${CMAKE_MATCH_1}")
    endif()
endif()
set(VERSION_L "${VERSION_SHORT} ${SIMBRIEF_HUB_COMMIT},xplib:${XPLIB_COMMIT}")
set(_vh "${CMAKE_CURRENT_SOURCE_DIR}/version.h")
set(_vh_content "// generated file!\n#define VERSION \"${VERSION_L}\"\n#define VERSION_SHORT \"${VERSION_SHORT}\"\n")
if(EXISTS "${_vh}")
    file(READ "${_vh}" _vh_existing)
    if(NOT _vh_existing STREQUAL _vh_content)
        file(WRITE "${_vh}" "${_vh_content}")
    endif()
else()
    file(WRITE "${_vh}" "${_vh_content}")
endif()

# ==============================================================================
# Sources
# ==============================================================================
set(SOURCES
    sbh.cpp
    ofp_get_parse.cpp
    cdm_get_parse.cpp
    ui.cpp
    ${XPLIB}/http_get.cpp
    ${XPLIB}/log_msg.cpp
)

SET(IMGUI_SRCS
    ${CMAKE_SOURCE_DIR}/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/imgui/misc/cpp/imgui_stdlib.cpp
    ${CMAKE_SOURCE_DIR}/ImgWindow/ImgWindow.cpp
    ${CMAKE_SOURCE_DIR}/ImgWindow/ImgFontAtlas.cpp
)

add_library(simbrief_hub SHARED ${SOURCES} ${IMGUI_SRCS})
set_target_properties(simbrief_hub PROPERTIES PREFIX "" SUFFIX ".xpl")

# ==============================================================================
# Common compile settings
# ==============================================================================
target_include_directories(simbrief_hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XPLIB}
    ${CMAKE_SOURCE_DIR}/imgui
    ${CMAKE_SOURCE_DIR}/imgui/misc/cpp
    ${CMAKE_SOURCE_DIR}/ImgWindow
    ${SDK}/CHeaders/XPLM
)

target_compile_definitions(simbrief_hub PRIVATE
    XPLM200 XPLM210 XPLM300 XPLM301
)

target_compile_options(simbrief_hub PRIVATE -O3)

# ==============================================================================
# Platform specifics
# ==============================================================================
if(WIN32)
    target_compile_definitions(simbrief_hub PRIVATE WINDOWS WIN32 IBM=1)
    target_compile_options(simbrief_hub PRIVATE
        -Wpedantic -Wextra -Wall -fdiagnostics-color -Wno-format-overflow -Wno-format-truncation
    )
    target_link_options(simbrief_hub PRIVATE
        -static-libgcc -static -Wl,--allow-multiple-definition
    )
    target_link_libraries(simbrief_hub PRIVATE
        ${SDK}/Libraries/Win/XPLM_64.lib
        winhttp
        opengl32
    )
    install(TARGETS simbrief_hub RUNTIME DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/simbrief_hub/win_x64")

elseif(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Universal binary")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "macOS deployment target")

    target_compile_definitions(simbrief_hub PRIVATE APL=1)
    target_compile_options(simbrief_hub PRIVATE
        -Wall -fPIC -fno-stack-protector -fvisibility=hidden -Wno-deprecated-declarations
    )
    target_link_options(simbrief_hub PRIVATE
        -rdynamic -fvisibility=hidden
        -Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/linkscript.mac
    )
    find_library(XPLM_FW XPLM PATHS "${SDK}/Libraries/Mac" REQUIRED)

    target_link_libraries(simbrief_hub PRIVATE ${XPLM_FW} curl "-framework OpenGL")
    install(TARGETS simbrief_hub LIBRARY DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/simbrief_hub/mac_x64")

elseif(UNIX)
    target_compile_definitions(simbrief_hub PRIVATE LIN=1)
    target_compile_options(simbrief_hub PRIVATE
        -Wall -Wextra -Wno-format-overflow -Wno-format-truncation
        -fPIC -fno-stack-protector
    )
    target_link_options(simbrief_hub PRIVATE -rdynamic -nodefaultlibs)
    target_link_libraries(simbrief_hub PRIVATE curl GL)
    install(TARGETS simbrief_hub LIBRARY DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/simbrief_hub/lin_x64")
endif()

# ==============================================================================
# Tests
# ==============================================================================
option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    # cdm_test
    add_executable(cdm_test
        cdm_test.cpp
        cdm_get_parse.cpp
        ${XPLIB}/http_get.cpp
        ${XPLIB}/log_msg.cpp
    )
    target_include_directories(cdm_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XPLIB}
        ${SDK}/CHeaders/XPLM
    )
    target_compile_definitions(cdm_test PRIVATE
        XPLM200 XPLM210 XPLM300 XPLM301 LOCAL_DEBUGSTRING
        $<IF:$<BOOL:${WIN32}>,WINDOWS WIN32 IBM=1,>
        $<IF:$<BOOL:${APPLE}>,APL=1,>
        $<IF:$<AND:$<BOOL:${UNIX}>,$<NOT:$<BOOL:${APPLE}>>>,LIN=1,>
    )
    target_compile_options(cdm_test PRIVATE -Wall -Wno-format-overflow)
    if(WIN32)
        target_link_libraries(cdm_test PRIVATE winhttp)
    else()
        target_link_libraries(cdm_test PRIVATE curl)
    endif()

    # ofp_test (compiled from ofp_get_parse.cpp with -DTEST_OFP_PARSE)
    # Since we also use ofp_get_parse.cpp in the library, we shouldn't modify the source file itself.
    # We compile it directly in the executable.
    add_executable(ofp_test
        ofp_get_parse.cpp
        ${XPLIB}/http_get.cpp
        ${XPLIB}/log_msg.cpp
    )
    target_include_directories(ofp_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XPLIB}
        ${SDK}/CHeaders/XPLM
    )
    target_compile_definitions(ofp_test PRIVATE
        TEST_OFP_PARSE
        XPLM200 XPLM210 XPLM300 XPLM301 LOCAL_DEBUGSTRING
        $<IF:$<BOOL:${WIN32}>,WINDOWS WIN32 IBM=1,>
        $<IF:$<BOOL:${APPLE}>,APL=1,>
        $<IF:$<AND:$<BOOL:${UNIX}>,$<NOT:$<BOOL:${APPLE}>>>,LIN=1,>
    )
    target_compile_options(ofp_test PRIVATE -Wall -Wno-format-overflow)
    if(WIN32)
        target_link_libraries(ofp_test PRIVATE winhttp)
    else()
        target_link_libraries(ofp_test PRIVATE curl)
    endif()
endif()
