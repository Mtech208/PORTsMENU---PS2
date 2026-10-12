# ps2_embed_romfs.cmake — embeber carpeta romfs/ como FS en el ELF (opcional)
# Uso: ps2_embed_romfs(OUTPUT_VAR ROMFS_DIR)

function(ps2_embed_romfs OUTPUT_VAR ROMFS_DIR)
    # Find genromfs tool (PC side)
    find_program(GENROMFS genromfs
        PATHS "${PS2SDK}/ee/fs/romfs/tools" "${PS2SDK}/tools" "/usr/bin" "/usr/local/bin"
        NO_DEFAULT_PATH
    )
    if(NOT GENROMFS)
        # Try to build from source if available
        if(EXISTS "${PS2SDK}/ee/fs/romfs/tools/genromfs/Makefile")
            execute_process(COMMAND make -C "${PS2SDK}/ee/fs/romfs/tools/genromfs"
                WORKING_DIRECTORY "${PS2SDK}/ee/fs/romfs/tools/genromfs"
                RESULT_VARIABLE res)
            if(res EQUAL 0 AND EXISTS "${PS2SDK}/ee/fs/romfs/tools/genromfs/genromfs")
                set(GENROMFS "${PS2SDK}/ee/fs/romfs/tools/genromfs/genromfs")
            endif()
        endif()
    endif()

    if(NOT GENROMFS)
        message(WARNING "genromfs no encontrado. Saltando embeber romfs (usará assets externos).")
        # Generate empty romfs_embed.c
        set(ROMFS_C "${CMAKE_CURRENT_BINARY_DIR}/romfs_embed.c")
        file(WRITE ${ROMFS_C}
            "// romfs embedding skipped (genromfs not found)\n"
            "#include <stdint.h>\n"
            "uint8_t romfs_img[] = { 0 };\n"
            "int size_romfs_img = 0;\n"
        )
        set(${OUTPUT_VAR} ${ROMFS_C} PARENT_SCOPE)
        return()
    endif()

    set(ROMFS_IMG "${CMAKE_CURRENT_BINARY_DIR}/romfs.img")
    set(ROMFS_C "${CMAKE_CURRENT_BINARY_DIR}/romfs_embed.c")

    # Generate romfs image from directory
    add_custom_command(
        OUTPUT ${ROMFS_IMG}
        COMMAND ${GENROMFS} -f ${ROMFS_IMG} -d ${ROMFS_DIR} -V "PORTMENU"
        COMMENT "Generando romfs.img desde ${ROMFS_DIR}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Convert to C array with bin2c
    find_program(BIN2C bin2c
        PATHS "${PS2SDK}/bin" "${PS2DEV_ROOT}/ps2sdk/bin"
        NO_DEFAULT_PATH
    )
    if(NOT BIN2C)
        message(FATAL_ERROR "bin2c no encontrado en PS2SDK/bin")
    endif()

    add_custom_command(
        OUTPUT ${ROMFS_C}
        COMMAND ${BIN2C} ${ROMFS_IMG} ${ROMFS_C} romfs_img
        COMMENT "Convirtiendo romfs.img a C array"
        DEPENDS ${ROMFS_IMG}
    )

    set(${OUTPUT_VAR} ${ROMFS_C} PARENT_SCOPE)
endfunction()