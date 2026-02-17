CXX := g++
CXXFLAGS := -std=c++11 -Wall -Wextra -O2
LDFLAGS :=

SRCS := patch.cpp
TARGET := patch

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
