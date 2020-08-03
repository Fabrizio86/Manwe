cmake_minimum_required(VERSION 3.16)
project(Yarns VERSION 3.1.2)

set(CMAKE_CXX_STANDARD 17)

add_library(Yarns
        itask.hpp
        iwaitable.hpp
        queue.hpp
        waitable.hpp
        waitable.cpp
        yarns.hpp
        yarns.cpp
        )
