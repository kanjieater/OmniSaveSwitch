#pragma once
// Minimal curl stub for host test builds.
#define CURL_ERROR_SIZE 256

// CURL is an opaque handle in libcurl. Tests never call real curl functions;
// http_*.cpp stubs live in fsm_deps.cpp.
typedef void CURL;
