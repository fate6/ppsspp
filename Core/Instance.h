// Copyright (c) 2020 PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include "ppsspp_config.h"

#include <cstdint>

extern uint8_t PPSSPP_ID;

void InitInstanceCounter();
void ShutdownInstanceCounter();
int GetInstancePeerCount();

#include <string>

inline bool IsFirstInstance() {
	return PPSSPP_ID == 1;
}

// psp2i fork: claims the config file (call once its path is known). Only one running PPSSPP can own a given
// ini, so separate installs (instance_b, its own memstick) each save their config even when they aren't the
// first instance.
void ClaimConfigFile(const std::string &iniPath);
// The first instance, or the one that owns this config file.
bool CanSaveConfig();
