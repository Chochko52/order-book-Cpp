cmake_minimum_required(VERSION 3.30)
project(OrderBookFinal)

set(CMAKE_CXX_STANDARD 26)

add_executable(OrderBookFinal main.cpp)

add_executable(OrderBookApp stress-test.cpp)