cmake_minimum_required(VERSION 3.5)

project(EpollClientServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions("-Wall -g")

include_directories(include)

add_executable(Client
	src/TxBuffer.cpp
	src/TxMsg.cpp
	Client.cpp)

add_executable(Server
	src/TxBuffer.cpp
	src/TxMsg.cpp
	Server.cpp)

target_link_libraries(Client pthread)
target_link_libraries(Server pthread)
