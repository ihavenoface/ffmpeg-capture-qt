cmake_minimum_required(VERSION 3.21)
project(ffmpeg_capture_recorder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(ffmpeg_capture_recorder
    WIN32
    main.cpp
)

target_link_libraries(ffmpeg_capture_recorder PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
