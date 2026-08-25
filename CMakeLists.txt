cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# espressif/libjpeg-turbo 3.1.1~2 still queries the pre-namespace component
# target while configuring its ExternalProject. ESP-IDF 6 exposes the managed
# component as idf::espressif__libjpeg-turbo instead. Supplying the legacy
# target keeps the upstream component compatible without modifying generated
# files under managed_components.
if(NOT TARGET idf::libjpeg-turbo)
    add_library(idf::libjpeg-turbo UNKNOWN IMPORTED GLOBAL)
    set_property(TARGET idf::libjpeg-turbo PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/main")
endif()

# The ESP-IDF libjpeg-turbo component forwards this variable to the nested
# ExternalProject build.  Keep the complete decompression pipeline explicit:
# progressive JPEGs need both the progressive entropy decoder and multiscan
# coefficient storage, while RGB565 output can select the merged upsampler.
# Merely defining D_PROGRESSIVE_SUPPORTED in application code is insufficient
# because these modules are compiled into libjpeg.a by the nested build.
set(extra_defines
    "-DD_MULTISCAN_FILES_SUPPORTED -DD_PROGRESSIVE_SUPPORTED -DD_LOSSLESS_SUPPORTED -DSAVE_MARKERS_SUPPORTED -DBLOCK_SMOOTHING_SUPPORTED -DIDCT_SCALING_SUPPORTED -DUPSAMPLE_MERGING_SUPPORTED -DQUANT_1PASS_SUPPORTED -DQUANT_2PASS_SUPPORTED -DDCT_ISLOW_SUPPORTED -DDCT_IFAST_SUPPORTED -DDCT_FLOAT_SUPPORTED"
    CACHE INTERNAL "libjpeg-turbo decoder capabilities")

project(lyra_firmware)

# Keep the rendering hot path optimized even when an existing Espressif-IDE
# workspace still has the generated sdkconfig set to the IDF debug preset.
# sdkconfig.defaults selects the same mode for fresh configurations.
# Managed components use their canonical namespace-qualified CMake name here;
# `lvgl` is only the dependency alias accepted by component manifests.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(board_lib lyra_board COMPONENT_LIB)
idf_component_get_property(main_lib main COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -O2)
target_compile_options(${board_lib} PRIVATE -O2)
target_compile_options(${main_lib} PRIVATE -O2)

# LVGL's ESP-IDF Kconfig omits this format switch in 9.4. Enable the supplied
# software renderer so persistent direct-mode buffers can stay in the panel's
# required MSB-first RGB565 byte order and never need a full-frame swap pass.
# Keep style caching enabled for existing generated configurations too.
target_compile_definitions(${lvgl_lib} PRIVATE
    LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=1
    LV_OBJ_STYLE_CACHE=1
)
