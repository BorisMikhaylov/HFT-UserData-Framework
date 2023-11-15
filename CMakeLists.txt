cmake_minimum_required(VERSION 3.24)
project(HFT_Framework_UserData)

set(CMAKE_CXX_STANDARD 23)

add_executable(HFT_Framework_UserData main.cpp
        easywsclient.cpp
        easywsclient.hpp
        bparser.cpp)