// Precompiled header for plugin_mc68030 DLL
// Stripped-down version of Em68030's pch.h (no WinRT/XAML dependencies)
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "shell32.lib")

#ifdef GetObject
#undef GetObject
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

// C++ standard library (matching Em68030 Core/IO requirements)
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <span>
#include <sstream>
#include <stdexcept>
#include <optional>
#include <cassert>
#include <bit>
#include <map>
#include <condition_variable>
