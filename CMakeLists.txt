cmake_minimum_required(VERSION 3.24)

# Get the Pico SDK from https://github.com/raspberrypi/pico-sdk.git tested on TAG 1.4.0
# Get FreeRTOS from https://github.com/FreeRTOS/FreeRTOS-Kernel.git tested on TAG V10.5.0
# Pico 2 W (-DPICO_BOARD=pico2_w) と SD カード (lib/FatFs_SD の pico/aon_timer) は pico-sdk 2.x が要る:
# -DPICO_SDK_PATH=<pico-sdk 2.1.1> -DFREERTOS_KERNEL_PATH=<raspberrypi/FreeRTOS-Kernel 4f7299d (RP2350 の port)>
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk CACHE PATH "pico-sdk")
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel CACHE PATH "FreeRTOS-Kernel")
set(LIBSMB2_PATH ${CMAKE_CURRENT_LIST_DIR}/libsmb2)

# Name the project and prepare it to be a pico_w project, using FreeRTOS and lwip
set(PROJECT_NAME x68kzremotedrv)
set(PICO_BOARD pico_w CACHE STRING "pico_w or pico2_w")
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
if(PICO_BOARD STREQUAL "pico2_w")
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)
else()
    include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)
endif()
project(${PROJECT_NAME} C CXX ASM)
pico_sdk_init()

# Add a couple of definitions that all projects/libraries can/should use
# Pico 2 W (RP2350): FreeRTOS のコア数 (-DP2W_CORES=1|2)。1 コアなら、フラッシュの書き込み (flash_safe_execute) が
# コア 1 を止めようとして NOT_PERMITTED (-4) にならないよう、コア 1 は安全と宣言する
if(PICO_PLATFORM MATCHES "rp2350")
    set(P2W_CORES 1 CACHE STRING "FreeRTOS cores on the RP2350 (1 or 2)")
    add_compile_definitions(P2W_CORES=${P2W_CORES})
    if(P2W_CORES EQUAL 1)
        add_compile_definitions(PICO_FLASH_ASSUME_CORE1_SAFE=1)
    endif()
endif()

add_definitions(-DPICO_PLATFORM=${PICO_PLATFORM})
add_definitions(-DHAVE_CONFIG_H)

# Build libsmb2 as a library
add_subdirectory(${LIBSMB2_PATH} libsmb2)
# SD カード (SPI) と FatFs: HDS を SD に置く (config の HDS を "sd:/ファイル名" にする)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/lib/FatFs_SD/src FatFs_SD)
# file_stream.c は fopencookie (GNU 拡張) を使う。使わないが、ライブラリがまとめて入れるのでここだけ GNU にする
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/lib/FatFs_SD/src/src/file_stream.c PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)

# Application, including in the FreeRTOS-Kernel
add_executable(
        ${PROJECT_NAME}
        src/main.c
        src/connect.c
        src/msc_disk.c
        src/virtual_disk.c
        src/vd_command.c
	src/hdscache.c
	src/smb2connect.c
        src/config_file.c
        src/netpipe.c
        src/sdhds.c
        src/sdfs.c
        src/hw_config.c
        src/usb_descriptors.c
        x68kserremote/service/remoteserv.c
        iconv/iconv_mini.c
)

add_custom_target(driver make
        WORKING_DIRECTORY ../driver
)
add_dependencies(${PROJECT_NAME} driver)

# Build the app
include_directories(
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/src/monocypher
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/driver
        ${CMAKE_CURRENT_LIST_DIR}/iconv
        ${CMAKE_CURRENT_LIST_DIR}/x68kserremote/include
        ${CMAKE_CURRENT_LIST_DIR}/x68kserremote/service
        ${FREERTOS_KERNEL_PATH}/include
        ${LIBSMB2_PATH}/include
        ${LIBSMB2_PATH}/include/smb2
        ${LIBSMB2_PATH}/include/picow
)

# 暗号アクセラレータ (src/accel.c, Monocypher)。-DACCEL_ENABLE=0 で外す (A/B 比較用)
option(ACCEL_ENABLE "X25519/Ed25519 accelerator for the X68000 Z over netpipe" ON)
if(ACCEL_ENABLE)
    target_sources(${PROJECT_NAME} PRIVATE
        src/accel.c
        src/accel_ops.c
        src/monocypher/monocypher.c
        src/monocypher/monocypher-ed25519.c
    )
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ACCEL_ENABLE=0)
endif()

# USB キーボード (src/hidkbd.c、UDP 6869 で picokbd と同じコマンドを受ける) を MSC に足す。-DHID_KEYBOARD=ON で入れる
option(HID_KEYBOARD "USB HID keyboard (remote typing over UDP 6869) next to MSC" OFF)
if(HID_KEYBOARD)
    target_sources(${PROJECT_NAME} PRIVATE
        src/hidkbd.c
        src/hidkbd_cmd.c
    )
    # UDP の口: DHCP, DNS, netpipe, hidkbd で既定の 4 を使い切るので余裕を持たせる
    target_compile_definitions(${PROJECT_NAME} PRIVATE HID_KEYBOARD=1 MEMP_NUM_UDP_PCB=6)
endif()

# tis.x -p (x68k リポジトリの net/tistack) のソケット窓口 (src/tsp_svc.c): netpipe の 0xA6 のレコードで Z のソケット操作を
# Pico の lwIP で行う (TCP / UDP / DNS)。-DTSP=OFF で外す。lwIP の大きさは src/tsp_lwip/lwipopts.h
option(TSP "Socket service for tis.x -p (TSP 0xA6 records over netpipe)" ON)
if(TSP)
    target_sources(${PROJECT_NAME} PRIVATE src/tsp_svc.c)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/tsp_lwip)
    # Pico W (264KB) に合わせて表を小さく: Pico 側ソケット 20 (Z の 32 番号を全部同時には使わない)、送信の合計 4KB
    if(PICO_PLATFORM MATCHES "rp2350")
        # Pico 2 W (520KB): 本来の大きさ (x68k net/tistack/README.md「Pico の RAM」)
        target_compile_definitions(${PROJECT_NAME} PRIVATE TSP_ENABLE=1 TSP_NPS=40 TSP_TXPOOL=12288 TSP_MEM_SIZE=20480)
    else()
        target_compile_definitions(${PROJECT_NAME} PRIVATE TSP_ENABLE=1 TSP_NPS=20 TSP_TXPOOL=4096)
    endif()
endif()

# HDS が開けるまで Z の読み書きに NOT READY (04/01) を返して待たせ、接続は裏で繰り直し続ける (0 埋めを返さない)。
# -DBOOTWAIT=OFF で元の動き (最初の HDS の読み込みで接続を待ち、失敗なら 0 埋め) に戻す
option(BOOTWAIT "Answer NOT READY until the HDS is open instead of blocking / returning zeros" ON)
if(BOOTWAIT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOOTWAIT=1)
endif()
# BOOTWAIT のとき、HDS の先頭 16KB をフラッシュ (0x1e0000) に控え、開けるまで待ち IPL で Z の画面に接続待ちを出す (src/bootwait.c)
option(BOOTWAIT_SCREEN "Show a waiting message on the Z while the HDS isn't open (needs BOOTWAIT)" ON)
if(BOOTWAIT AND BOOTWAIT_SCREEN)
    target_sources(${PROJECT_NAME} PRIVATE src/bootwait.c src/bootwait_pico.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOOTWAIT_SCREEN=1)
    target_link_libraries(${PROJECT_NAME} pico_flash)
endif()

# 起動時の調査用: Z からの MSC の命令を 1 件 1 行の UDP で MSC_LOG_HOST:6870 に送る (src/msclog.c)。-DMSC_LOG=ON で入れる
option(MSC_LOG "Log USB MSC commands over UDP (boot-time investigation)" OFF)
set(MSC_LOG_HOST "192.168.1.108" CACHE STRING "Receiver of the MSC log (UDP 6870)")
if(MSC_LOG)
    target_sources(${PROJECT_NAME} PRIVATE src/msclog.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MSC_LOG=1 MSC_LOG_HOST="${MSC_LOG_HOST}")
endif()

pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_enable_stdio_semihosting(${PROJECT_NAME} 0)

execute_process(COMMAND git describe --tags --always
        OUTPUT_VARIABLE GIT_REPO_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_compile_definitions(-DGIT_REPO_VERSION="${GIT_REPO_VERSION}")

target_compile_definitions(${PROJECT_NAME} PRIVATE
        PICO_HEAP_SIZE=0x18000
        NO_SYS=0            # don't want NO_SYS (generally this would be in your lwipopts.h)
)

target_compile_options(${PROJECT_NAME} PRIVATE -g)

target_link_libraries(${PROJECT_NAME}
        pico_cyw43_arch_lwip_sys_freertos
        FreeRTOS-Kernel
        pico_stdlib
        libsmb2
        FreeRTOS-Kernel-Heap3
        tinyusb_device
        tinyusb_board
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

pico_add_extra_outputs(${PROJECT_NAME})
