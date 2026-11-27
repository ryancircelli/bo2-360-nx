# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests; need the powerpc-none-elf binutils (REXGLUE_PPC_BINUTILS_DIR) or prebuilt bins
option(REXGLUE_BUILD_PPC_TESTS "Build the PPC instruction tests" OFF)
if(REXGLUE_BUILD_PPC_TESTS)
    add_subdirectory(ppc)
endif()
