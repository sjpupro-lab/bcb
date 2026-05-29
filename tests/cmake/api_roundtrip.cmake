# BCB — CTest driver (cross-platform via `cmake -P`).
# Builds a prior from a corpus, then runs a public-API test binary (linked
# against the SHARED library) to verify dynamic-lib round-trip losslessness.
#
# Required -D vars: PRIOR_BUILD, TEST_BIN, CORPUS, WORKDIR, MODE(landmark|schema)
# Optional: TEST_ARGS (list appended after <prior> <msg>), RECORD_SIZE (schema)

if(NOT PRIOR_BUILD OR NOT TEST_BIN OR NOT CORPUS OR NOT WORKDIR)
  message(FATAL_ERROR "api_roundtrip.cmake: missing required -D arguments")
endif()

set(prior "${WORKDIR}/ctest_api.bcb-prior")
set(msg   "${WORKDIR}/ctest_api_msg.bin")

# Small message slice from the corpus (keep thread test fast).
file(READ "${CORPUS}" _slice LIMIT 2048)
file(WRITE "${msg}" "${_slice}")

if(MODE STREQUAL "schema")
  if(NOT RECORD_SIZE)
    set(RECORD_SIZE 18)
  endif()
  set(build_opts --schema-record-size ${RECORD_SIZE})
else()
  set(build_opts --landmark-k 256)
endif()

execute_process(
  COMMAND "${PRIOR_BUILD}" "${CORPUS}" "${prior}" --train-size 50000 ${build_opts}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "prior build failed (${rc}):\n${out}\n${err}")
endif()

execute_process(
  COMMAND "${TEST_BIN}" "${prior}" "${msg}" ${TEST_ARGS}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "test failed (${rc}):\n${out}\n${err}")
endif()
