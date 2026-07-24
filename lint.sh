#!/bin/bash
clang-format -i src/*.c
clang-format -i src/**/*.c
clang-format -i src/**/**/*.c
clang-format -i include/**/*.h
clang-format -i include/**/**/*.h
clang-format -i include/**/**/**/*.h
clang-format -i tests/unit/src/*.cpp
