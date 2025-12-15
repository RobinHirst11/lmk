cmake_minimum_required(VERSION 3.20)
project(lmk)

find_package(PkgConfig REQUIRED)
find_package(Freetype REQUIRED)
find_package(X11 REQUIRED)

pkg_check_modules(XFT REQUIRED xft)

add_subdirectory(./src)
