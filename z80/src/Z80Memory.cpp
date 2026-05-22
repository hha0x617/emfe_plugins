// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Memory.cpp - 64 KiB flat memory. Phase 1.1 scaffold only.

#include "pch.h"
#include "Z80Memory.h"

void Z80Memory::Reset() {
    Ram.fill(0);
}
