cmake_minimum_required(VERSION 3.24)
project(ttkConverter VERSION 1.1.1 LANGUAGES CXX RC)

option(TTK_MODERN "Enable modern Windows hardware encoding controls" OFF)
add_executable(ttkConverter WIN32
    src/main.cpp src/core.cpp src/media.cpp src/pdf.cpp src/app.rc
)
target_compile_features(ttkConverter PRIVATE cxx_std_17)
target_compile_definitions(ttkConverter PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601
    TTK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TTK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TTK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    TTK_VERSION_TEXT=${PROJECT_VERSION}
)
if(TTK_MODERN)
    target_compile_definitions(ttkConverter PRIVATE TTK_MODERN)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(ttkConverter PRIVATE TTK_LEGACY_X86)
endif()
target_include_directories(ttkConverter PRIVATE src)
target_link_libraries(ttkConverter PRIVATE comctl32 comdlg32 shell32 user32 gdi32 ole32 shlwapi windowscodecs uuid)

if(MSVC)
    set_property(TARGET ttkConverter PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(ttkConverter PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4>)
    target_compile_options(ttkConverter PRIVATE $<$<COMPILE_LANGUAGE:RC>:/c65001>)
elseif(MINGW)
    target_compile_options(ttkConverter PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-ffunction-sections;-fdata-sections>)
    target_compile_options(ttkConverter PRIVATE $<$<COMPILE_LANGUAGE:RC>:--codepage=65001>)
    target_link_options(ttkConverter PRIVATE -municode -static -Wl,--gc-sections -Wl,--major-subsystem-version,6,--minor-subsystem-version,1)
endif()
