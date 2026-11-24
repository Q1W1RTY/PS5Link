
add_library(munit "${CMAKE_CURRENT_SOURCE_DIR}/munit/munit.c")
target_include_directories(munit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/munit")

# MinGW's tmpfile() fails in some Windows/MSYS environments. MUnit uses it
# only to buffer test stderr, so disable that optional capture on Windows.
if(WIN32)
	target_compile_definitions(munit PRIVATE MUNIT_NO_BUFFER)
endif()

set(CHIAKI_UNIT_SOURCES
				main.c
				http.c
				rpcrypt.c
				gkcrypt.c
				takion.c
				seqnum.c
				keystate.c
				feedback.c
				reorderqueue.c
				fec.c
				test_log.c
				test_log.h
				bitstream.c
				regist.c)

if(NOT CHIAKI_LIB_ENABLE_MBEDTLS)
	list(APPEND CHIAKI_UNIT_SOURCES aia.c)
endif()

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	list(APPEND CHIAKI_UNIT_SOURCES ffmpegdecoder.c)
endif()

add_executable(chiaki-unit ${CHIAKI_UNIT_SOURCES})
add_test(NAME unit COMMAND chiaki-unit)

target_link_libraries(chiaki-unit chiaki-lib munit)

if(CHIAKI_ENABLE_FFMPEG_DECODER)
	target_link_libraries(chiaki-unit FFMPEG::avcodec FFMPEG::avutil)
endif()

# Link-time feedback stubs keep the receiver tests entirely offline.
if(PS5LINK_BUILD AND MINGW)
    add_executable(ps5link-receiver-test videoreceiver.c)
    target_link_libraries(ps5link-receiver-test chiaki-lib munit)
    target_link_options(ps5link-receiver-test PRIVATE
        "-Wl,--wrap=stream_connection_send_corrupt_frame"
        "-Wl,--wrap=stream_connection_send_idr_request"
        "-Wl,--wrap=chiaki_time_now_monotonic_us")
    add_test(NAME ps5link-receiver COMMAND ps5link-receiver-test)
endif()


if(PS5LINK_BUILD)
    add_executable(ps5link-controller-idle-test ds5btidle.cpp)
    target_compile_features(ps5link-controller-idle-test PRIVATE cxx_std_17)
    target_include_directories(ps5link-controller-idle-test PRIVATE "${CMAKE_SOURCE_DIR}/gui/include")
    add_test(NAME ps5link-controller-idle COMMAND ps5link-controller-idle-test)
endif()
