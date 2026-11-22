TARGET = dialtone
TYPE = ps-exe

# the engine is built with the same BUILD, so switching it needs a `make clean`
BUILD ?= ReleaseFast

CPPFLAGS += -Imadnight_engine/src
CPPFLAGS += -Imadnight_engine/third_party/nugget

rwildcard=$(wildcard $1$2) $(foreach d,$(wildcard $1*),$(call rwildcard,$d/,$2))
SRCS := $(call rwildcard,src/,*.cpp)

CXXFLAGS = -std=c++20

include madnight_engine/madnight-engine.mk
