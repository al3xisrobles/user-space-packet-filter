# Compiler and flags
CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -pthread -Iinclude
LDFLAGS := -lpcap   # Link against libpcap for packet capture

# Project directories
SRC_DIR := src
INCLUDE_DIR := include
TEST_DIR := tests
BUILD_DIR := build

# Source files
SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
DEPS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.d, $(SRCS))

# Test files
TEST_SRCS := $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS := $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(TEST_SRCS))

# Executable name
TARGET := fast-packet-filter
TEST_TARGET := test-runner

# Default rule (compiles everything)
all: $(TARGET)

# Compile the main executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

# Run tests
test: $(TEST_TARGET)
	./$(TEST_TARGET)

# Compile test runner
$(TEST_TARGET): $(TEST_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Clean build files
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_TARGET)

# Include dependency files for incremental builds
-include $(DEPS)
