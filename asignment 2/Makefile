CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2
SOURCES = framework_api.cc design.cc \
	framework/machine_lifecycle.cc framework/machine_fetch.cc framework/machine_data.cc \
	framework/machine_retirement.cc framework/machine_reporting.cc \
	framework/builder.cc framework/builder_build.cc framework/builder_validation.cc \
	framework/processor.cc framework/processor_reporting.cc framework/main.cc \
	design/instruction_helpers.cc design/control.cc design/core.cc design/adapters.cc \
	execution_unit.cc execution_unit/helpers.cc execution_unit/impl.cc \
	cache/cache_config.cc cache/replacement.cc cache/cache_controller.cc framework/simple_memory.cc
HEADERS = pipesim.h execution_config.h execution_unit.h cache/cache_config.h cache/cache_types.h \
	cache/replacement.h cache/cache_controller.h cache/cache_controller/internal.h framework/simple_memory.h \
	design/instruction_helpers.h design/control.h design/core.h design/adapters.h \
	execution_unit/helpers.h execution_unit/internal.h framework/internal.h

all: pipesim

pipesim: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -I. $(SOURCES) -o pipesim

clean:
	rm -f pipesim tutorial tests/unit_tests

.PHONY: all clean test

test: pipesim
	./tests/run_tests.sh
