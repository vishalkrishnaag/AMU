CXX = g++
CXXFLAGS = -std=c++17 -O3 -march=native
CPPFLAGS = -Ithird_party/eigen -I/usr/include/eigen3
LDFLAGS =

SRC = main.cpp VM.cpp Lexer.cpp Parser.cpp FunctionLoader.cpp
TARGET = intense.out

EIGEN_VERSION = 3.4.0
EIGEN_ARCHIVE = eigen-$(EIGEN_VERSION).tar.gz
EIGEN_URL = https://gitlab.com/libeigen/eigen/-/archive/$(EIGEN_VERSION)/$(EIGEN_ARCHIVE)
EIGEN_DIR = third_party/eigen

.PHONY: all deps clean

all: deps $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $(TARGET)

deps: $(EIGEN_DIR)/Eigen/Core

$(EIGEN_DIR)/Eigen/Core:
	@mkdir -p third_party
	@if [ -f /usr/include/eigen3/Eigen/Core ]; then \
		echo "Using system Eigen from /usr/include/eigen3"; \
	elif [ -f "$(EIGEN_DIR)/Eigen/Core" ]; then \
		echo "Using vendored Eigen from $(EIGEN_DIR)"; \
	else \
		echo "Fetching Eigen $(EIGEN_VERSION) into third_party/"; \
		curl -L "$(EIGEN_URL)" -o "third_party/$(EIGEN_ARCHIVE)"; \
		tar -xzf "third_party/$(EIGEN_ARCHIVE)" -C third_party; \
		rm -rf "$(EIGEN_DIR)"; \
		mv "third_party/eigen-$(EIGEN_VERSION)" "$(EIGEN_DIR)"; \
		rm -f "third_party/$(EIGEN_ARCHIVE)"; \
	fi

clean:
	rm -f $(TARGET)
