# Copyright (c) 2026 Johannes Overmann
#
# Distributed under the Boost Software License, Version 1.0.

CPPFLAGS ?= -pedantic
CXXSTD ?= -std=c++23
UNAME_S := $(shell uname -s)
ifeq ($(origin CXX),default)
ifeq ($(UNAME_S),Linux)
CXX = clang++-18
else
CXX = g++
endif
endif
BUILD ?= release
CXXFLAGS_COMMON ?= -Wall -Wextra
CXXFLAGS_DEBUG ?= -O0 -g
CXXFLAGS_RELEASE ?= -O3 -DNDEBUG
PYTHON ?= python3

ifeq ($(BUILD),debug)
CXXFLAGS ?= $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG)
else ifeq ($(BUILD),release)
CXXFLAGS ?= $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE)
else
$(error Unknown BUILD='$(BUILD)', expected debug or release)
endif

BUILDDIR=build-$(BUILD)
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:%.cpp=$(BUILDDIR)/%.o)
DEPENDS := $(SOURCES:%.cpp=$(BUILDDIR)/%.d)
TARGET = tbindiff

default: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $^ -o $@
	@echo "Done."

$(BUILDDIR)/%.o: %.cpp $(BUILDDIR)/%.d
	$(CXX) $(CXXSTD) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.d: %.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CXXSTD) $(CPPFLAGS) -MM -MQ $@ $< -o $@

clean:
	rm -rf build-* $(TARGET)

test: $(TARGET)
	$(PYTHON) test/test_tbindiff.py

format:
	clang-format -i --style=file src/*.hpp src/*.cpp

.PHONY: clean default test format

ifeq ($(findstring $(MAKECMDGOALS),clean),)
-include $(DEPENDS)
endif
