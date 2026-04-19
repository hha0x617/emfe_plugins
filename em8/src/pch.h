// Precompiled header for plugin_em8 DLL
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef GetObject
#undef GetObject
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <algorithm>
#include <format>
#include <memory>
#include <queue>
#include <chrono>
#include <condition_variable>
