cmake_minimum_required(VERSION 3.16)
project(HFT_Framework_UserData)

set(CMAKE_CXX_STANDARD 17)

add_executable(HFT_Framework_UserData main.cpp
        easywsclient.cpp
        easywsclient.hpp
        fastsocket.h
        fastsocket.cpp
)