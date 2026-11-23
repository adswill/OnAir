# KAVL, the narrow-band layered video mode (spec: scratchpad/KAVL-SPEC-v0.1.md). Developed as a separate program next to OnAir until it is finished;
# it links the OnAir core for the shared pieces (QAM, CRC, convolutional code, ATSC 3.0 LDPC, resampler). Added from the top CMakeLists.txt.
add_library(kavl STATIC src/kavl_phy.cpp src/kavl_rx.cpp)
target_include_directories(kavl PUBLIC include PRIVATE ${ONAIR_ROOT}/core/src)
target_link_libraries(kavl PUBLIC dect2core)

add_executable(kavl_iqwav tools/kavl_iqwav.cpp)   # a demonstration IQ .wav (opens in SDR++)
target_link_libraries(kavl_iqwav PRIVATE kavl)

foreach(t phy thresholds)
  add_executable(test_kavl_${t} tests/test_kavl_${t}.cpp)
  target_include_directories(test_kavl_${t} PRIVATE tests)
  target_link_libraries(test_kavl_${t} PRIVATE kavl)
  add_test(NAME kavl_${t} COMMAND test_kavl_${t})
endforeach()
set_tests_properties(kavl_phy PROPERTIES TIMEOUT 600)
