cmake_minimum_required(VERSION 3.20)

project(PORTMENU LANGUAGES C ASM)

# Logging
if(NOT DEFINED PORTMENU_LOG_LEVEL)
    set(PORTMENU_LOG_LEVEL 1 CACHE STRING "Logging verbosity (0=off, 1=normal, 2=verbose)")
endif()
set_property(CACHE PORTMENU_LOG_LEVEL PROPERTY STRINGS 0 1 2)
if(NOT PORTMENU_LOG_LEVEL MATCHES "^[012]$")
    message(FATAL_ERROR "PORTMENU_LOG_LEVEL must be 0, 1, or 2")
endif()

# Try without LTO (ports libs incompatible with LTO)
# gsKit may work without LTO if linked properly

# Sources
file(GLOB SOURCES
    src/platform/ps2/*.c
    src/platform/ps2/*.cpp
)

add_executable(PORTMENU ${SOURCES})
set_target_properties(PORTMENU PROPERTIES OUTPUT_NAME "PORTMENU" SUFFIX ".elf")

target_include_directories(PORTMENU PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/platform/ps2
    ${PS2GSKIT}/include
    ${PS2SDK}/ee/include
    ${PS2SDK}/ports/include
)

target_compile_definitions(PORTMENU PRIVATE
    PS2_PLATFORM
    PORTMENU_LOG_LEVEL=${PORTMENU_LOG_LEVEL}
    _EE=1
)

# Core PS2 libs - fully manual GS init + DMA/GIF
target_link_libraries(PORTMENU
    dma
    pad
    kernel
    cglue
    c
    m
)

# Link directories
target_link_directories(PORTMENU PRIVATE
    ${PS2GSKIT}/lib
    ${PS2SDK}/ee/lib
    ${PS2SDK}/ports/lib
    ${PS2SDK}/ports/lib/static
    ${PS2EE_LIB}
)

# Link flags
target_link_options(PORTMENU PRIVATE
    -Wl,--gc-sections
)

# Post-build: save unstripped + strip
find_program(PS2_STRIP mips64r5900el-ps2-elf-strip)
if(PS2_STRIP)
    add_custom_command(TARGET PORTMENU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:PORTMENU> $<TARGET_FILE_DIR:PORTMENU>/PORTMENU_syms.elf
        COMMAND ${PS2_STRIP} --strip-all $<TARGET_FILE:PORTMENU>
        COMMENT "Saving PORTMENU_syms.elf + stripping PORTMENU.elf"
    )
endif()

# Optional pack
find_program(PS2_PACKER ps2-packer
    PATHS "${PS2DEV_ROOT}/bin" "${PS2SDK}/bin"
    NO_DEFAULT_PATH
)
if(PS2_PACKER)
    add_custom_command(TARGET PORTMENU POST_BUILD
        COMMAND ${PS2_PACKER} $<TARGET_FILE:PORTMENU> $<TARGET_FILE_DIR:PORTMENU>/PORTMENU_packed.elf
        COMMENT "Packing PORTMENU.elf with ps2-packer"
    )
endif()