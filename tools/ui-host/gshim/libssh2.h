// ui-host gallery shim: the real libssh2.h, seen as on the device (no Winsock: lwIP's sockets, gshim/lwip).
#pragma once
#pragma push_macro("_WIN32")
#undef _WIN32
#include_next <libssh2.h>
#pragma pop_macro("_WIN32")
