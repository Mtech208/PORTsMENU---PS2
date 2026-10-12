# ps2_embed_irx.cmake — embeber módulos IRX como arrays C
# Uso: ps2_embed_irx(OUTPUT_VAR IRX1 [IRX2 ...])

function(ps2_embed_irx OUTPUT_VAR)
    find_program(BIN2C bin2c
        PATHS "${PS2SDK}/bin" "${PS2DEV_ROOT}/ps2sdk/bin"
        NO_DEFAULT_PATH
    )
    if(NOT BIN2C)
        message(FATAL_ERROR "bin2c no encontrado")
    endif()

    set(IRX_BLOBS_C "${CMAKE_CURRENT_BINARY_DIR}/ps2_irx_blobs.c")
    set(IRX_SRCS)

    foreach(IRX_FILE ${ARGN})
        if(EXISTS "${IRX_FILE}")
            get_filename_component(IRX_NAME ${IRX_FILE} NAME_WE)
            set(IRX_C "${CMAKE_CURRENT_BINARY_DIR}/${IRX_NAME}_irx.c")
            add_custom_command(
                OUTPUT ${IRX_C}
                COMMAND ${BIN2C} ${IRX_FILE} ${IRX_C} ${IRX_NAME}_irx
                COMMENT "Embedding ${IRX_NAME}.irx"
            )
            list(APPEND IRX_SRCS ${IRX_C})
        else()
            message(WARNING "IRX no encontrado: ${IRX_FILE}")
        endif()
    endforeach()

    # Generate combined blobs file
    file(WRITE ${IRX_BLOBS_C}
        "// Auto-generated IRX blobs\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n\n"
    )
    foreach(IRX_FILE ${ARGN})
        if(EXISTS "${IRX_FILE}")
            get_filename_component(IRX_NAME ${IRX_FILE} NAME_WE)
            file(APPEND ${IRX_BLOBS_C}
                "extern uint8_t ${IRX_NAME}_irx[];\n"
                "extern int size_${IRX_NAME}_irx;\n"
            )
        endif()
    endforeach()
    file(APPEND ${IRX_BLOBS_C}
        "\n// Module registration for fileXio/cdfs/etc\n"
        "// Use with: SifExecModuleBuffer(${IRX_NAME}_irx, size_${IRX_NAME}_irx, 0, NULL, &res);\n"
    )

    set(${OUTPUT_VAR} ${IRX_BLOBS_C} PARENT_SCOPE)
endfunction()