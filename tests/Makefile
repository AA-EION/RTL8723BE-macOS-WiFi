CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -I. -Imock

SRCS = \
    mock/mock_crypto.cpp \
    mock/mock_pci_mmio.cpp \
    mock/mock_dma.cpp \
    mock/mock_packet_injector.cpp \
    mock/simulated_device.cpp \
    tier1_features/test_firmware_loader.cpp \
    tier1_features/test_efuse_parser.cpp \
    tier1_features/test_dma_ring.cpp \
    tier1_features/test_beacon_parser.cpp \
    tier1_features/test_wpa2_handshake.cpp \
    tier1_features/test_ccmp_crypto.cpp \
    tier2_boundaries/test_boundaries.cpp \
    tier3_pairwise/test_pairwise.cpp \
    tier4_workloads/test_workloads.cpp \
    test_runner.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = test_runner

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
