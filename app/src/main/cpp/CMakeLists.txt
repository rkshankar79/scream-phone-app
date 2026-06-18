cmake_minimum_required(VERSION 3.22.1)

project(screamtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Builds libscream.so:
#   scream_core/   - untouched Ericsson SCReAM sources
#   controllers/   - ScreamV2Tx -> CongestionController adapter
#   wrapper/       - UDP/threads session + JNI entry points
add_library(scream SHARED
    scream_core/RtpQueue.cpp
    scream_core/ScreamTx.cpp
    scream_core/ScreamV2Tx.cpp
    scream_core/ScreamV2TxStream.cpp
    scream_core/ScreamRx.cpp
    controllers/ScreamController.cpp
    controllers/ScreamReceiver.cpp
    wrapper/TestSession.cpp
    wrapper/ReceiverSession.cpp
    wrapper/scream_sender_jni.cpp
    wrapper/scream_receiver_jni.cpp)

# SCReAM V2 selection flag used throughout scream_core.
target_compile_definitions(scream PRIVATE V2)

target_include_directories(scream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/scream_core)

# Quiet the vendored sources without touching them.
target_compile_options(scream PRIVATE -Wno-write-strings -Wno-format)

find_library(log-lib log)
target_link_libraries(scream ${log-lib})
