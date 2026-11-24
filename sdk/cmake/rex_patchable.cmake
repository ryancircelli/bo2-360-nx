# rexglue_patchable_all(<recomp target>...): with REXGLUE_PATCHABLE_ALL=ON (test builds only, default OFF) every
# recompiled function of the targets gets a hot-swap slot, so a patch library (rex/system/patch.h,
# sdk/tools/build_patch_lib.sh) can replace any of them in a running game. Puts rex/system/patchable_all.h ahead of
# the generated PCH; with the option OFF nothing changes and the build is the same as without this call.
option(REXGLUE_PATCHABLE_ALL "Test builds: every recompiled function can be hot-swapped by a patch library" OFF)
# CACHE INTERNAL: games call the function from their own directory, where a normal variable of the SDK is not seen
set(REXGLUE_PATCHABLE_ALL_HEADER "${CMAKE_CURRENT_LIST_DIR}/../include/rex/system/patchable_all.h" CACHE INTERNAL "")

function(rexglue_patchable_all)
    if(NOT REXGLUE_PATCHABLE_ALL)
        return()
    endif()
    if(NOT APPLE)
        message(FATAL_ERROR "REXGLUE_PATCHABLE_ALL is implemented for macOS only")
    endif()
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(headers ${target} PRECOMPILE_HEADERS)
        if(NOT headers)
            set(headers "")
        endif()
        list(REMOVE_ITEM headers "${REXGLUE_PATCHABLE_ALL_HEADER}")
        set_property(TARGET ${target} PROPERTY PRECOMPILE_HEADERS "${REXGLUE_PATCHABLE_ALL_HEADER};${headers}")
        get_target_property(headers ${target} PRECOMPILE_HEADERS)
        list(GET headers 0 first)
        if(NOT first STREQUAL REXGLUE_PATCHABLE_ALL_HEADER OR NOT EXISTS "${first}")
            message(FATAL_ERROR "rexglue_patchable_all: ${target} did not get ${REXGLUE_PATCHABLE_ALL_HEADER}")
        endif()
        message(STATUS "${target}: every recompiled function is patchable (REXGLUE_PATCHABLE_ALL, ${first})")
    endforeach()
endfunction()
