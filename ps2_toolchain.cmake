# PS2 toolchain for PORTsMENUPS2
# Based on mkds-ps2 / pvz-ps2-wii ps2_toolchain.cmake
# Requires PS2DEV pointing to ps2dev root (with ee/, iop/, dvp/, gsKit/, ps2sdk)

cmake_minimum_required(VERSION 3.20)

# Force CMake to use Windows cmd.exe as shell (not MSYS2 /bin/sh)
set(CMAKE_SH "C:/Windows/System32/cmd.exe" CACHE INTERNAL "Shell for execute_process()")

# Pass PS2DEV to try_compile subprocesses
set(CMAKE_TRY_COMPILE_ENVIRONMENT "PS2DEV;PS2SDK" CACHE INTERNAL "Environment variables to pass to try_compile")

# Hardcoded paths for PS2DEV (bypass environment variable issues in try_compile)
set(PS2DEV_ROOT "C:/Users/gamer/OneDrive/Desktop/ps2dev-official/ps2dev" CACHE PATH "PS2DEV root directory")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips64r5900el)

set(PS2SDK "${PS2DEV_ROOT}/ps2sdk")
set(PS2SDK_COMMON_INC "${PS2DEV_ROOT}/common/include")
set(PS2EE_BIN "${PS2DEV_ROOT}/ee/bin")
set(PS2IOP_BIN "${PS2DEV_ROOT}/iop/bin")
set(PS2DVP_BIN "${PS2DEV_ROOT}/dvp/bin")
set(PS2GSKIT "${PS2DEV_ROOT}/gsKit")
set(PS2EE_INC "${PS2DEV_ROOT}/ee/mips64r5900el-ps2-elf/include")
set(PS2EE_GCC_INC "${PS2DEV_ROOT}/ee/lib/gcc/mips64r5900el-ps2-elf/15.2.0/include")
set(PS2EE_LIB "${PS2DEV_ROOT}/ee/mips64r5900el-ps2-elf/lib")

# Cross-compiler
set(CMAKE_C_COMPILER "${PS2EE_BIN}/mips64r5900el-ps2-elf-gcc.exe")
set(CMAKE_CXX_COMPILER "${PS2EE_BIN}/mips64r5900el-ps2-elf-g++.exe")
set(CMAKE_ASM_COMPILER "${PS2EE_BIN}/mips64r5900el-ps2-elf-as-wrapper.sh")
set(CMAKE_AR "${PS2EE_BIN}/mips64r5900el-ps2-elf-ar.exe")
set(CMAKE_RANLIB "${PS2EE_BIN}/mips64r5900el-ps2-elf-ranlib.exe")
set(CMAKE_STRIP "${PS2EE_BIN}/mips64r5900el-ps2-elf-strip.exe")
set(CMAKE_OBJCOPY "${PS2EE_BIN}/mips64r5900el-ps2-elf-objcopy.exe")
set(CMAKE_OBJDUMP "${PS2EE_BIN}/mips64r5900el-ps2-elf-objdump.exe")

# Force CMake to use these
set(CMAKE_C_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
set(CMAKE_C_COMPILER_ID "GNU" CACHE STRING "" FORCE)
set(CMAKE_C_COMPILER_VERSION "15.2.0" CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD_COMPUTED_DEFAULT "17" CACHE STRING "" FORCE)
set(CMAKE_C_EXTENSIONS_COMPUTED_DEFAULT "ON" CACHE STRING "" FORCE)

set(CMAKE_CXX_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
set(CMAKE_CXX_COMPILER_ID "GNU" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_VERSION "15.2.0" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_COMPUTED_DEFAULT "20" CACHE STRING "" FORCE)
set(CMAKE_CXX_EXTENSIONS_COMPUTED_DEFAULT "ON" CACHE STRING "" FORCE)

# Target flags for EE (PS2 Emotion Engine) - NO -fno-lto here, let CMakeLists handle LTO
set(CMAKE_C_FLAGS_INIT "-O2 -ffunction-sections -fdata-sections -fno-exceptions -march=r5900 -mhard-float -fno-pic -mno-shared -mabi=n32 -std=c99 -I${PS2EE_INC} -I${PS2EE_GCC_INC} -I${PS2SDK}/ee/include -I${PS2SDK_COMMON_INC}")
set(CMAKE_CXX_FLAGS_INIT "-O2 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -march=r5900 -mhard-float -fno-pic -mno-shared -mabi=n32 -std=c++20 -I${PS2EE_INC} -I${PS2EE_GCC_INC} -I${PS2SDK}/ee/include -I${PS2SDK_COMMON_INC}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} -EL")
set(CMAKE_ASM_FLAGS_INIT "-G0 -EL -I${PS2EE_INC} -I${PS2SDK}/ee/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--gc-sections -L${PS2EE_LIB} -L${PS2SDK}/ee/lib")

# PS2-specific definitions
set(PS2 ON CACHE BOOL "Building for PS2" FORCE)
add_compile_definitions(
    PS2=1
    EE_CORE=1
    _EE=1
    _LANGUAGE_C_PLUS_PLUS
    _MIPS_SZLONG=32
    _MIPS_SZINT=32
    _MIPS_SZPTR=32
)

# Find ps2sdk libraries
find_library(PS2_GSKIT gskit PATHS "${PS2GSKIT}/lib" NO_DEFAULT_PATH)
find_library(PS2_GSKIT_TOOLKIT gskit_toolkit PATHS "${PS2GSKIT}/lib" NO_DEFAULT_PATH)
find_library(PS2_DMAKIT dmakit PATHS "${PS2GSKIT}/lib" NO_DEFAULT_PATH)
find_library(PS2_PAD pad PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_MOUSE mouse PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_GRAPH graph PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_DMA dma PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_VUX vux PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_EEDEBUG eedebug PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_DEBUG debug PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_KERNEL kernel PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_PATCHES patches PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_AUDSRV audsrv PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_FILEXIO fileXio PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_CDVD cdvd PATHS "${PS2SDK}/ee/lib" NO_DEFAULT_PATH)
find_library(PS2_M m PATHS "${PS2EE_LIB}" NO_DEFAULT_PATH)
find_library(PS2_ROMFS romfs PATHS "${PS2SDK}/ports/lib" NO_DEFAULT_PATH)
find_library(PS2_Z z PATHS "${PS2SDK}/ports/lib" NO_DEFAULT_PATH)
find_library(PS2_PNG png PATHS "${PS2SDK}/ports/lib" NO_DEFAULT_PATH)
find_library(PS2_JPEG jpeg PATHS "${PS2SDK}/ports/lib" NO_DEFAULT_PATH)

# Embedded IRX modules directory
set(PS2_EMBED_IRX_DIR "${PS2SDK}/ee/irx")

# Disable features not available on PS2
set(NDS_SDL_BACKEND "NONE" CACHE STRING "Force NONE for PS2" FORCE)
set(NDS_ENABLE_COMPUTE_RENDERER OFF CACHE BOOL "Disable compute renderer for PS2" FORCE)
set(NDS_ENABLE_NETWORK OFF CACHE BOOL "Disable libslirp/networking for PS2" FORCE)
set(NDS_ENABLE_PCAP_BACKEND OFF CACHE BOOL "Disable pcap backend for PS2" FORCE)