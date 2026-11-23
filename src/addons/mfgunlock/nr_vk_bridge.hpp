// SPDX-License-Identifier: MIT
#pragma once
// 1.12.0 (DLSS 5, phase E): while DLSS 5's vkCreateDevice hook (nr_vulkan.hpp) forwards the game's call,
// the next hook in this same add-on (XeMFG Vulkan, native_vulkan.cpp) is to treat it as the game's: its
// check for calls from the add-on itself would otherwise skip XeMFG's device setup (RDR2: no frame
// generation once DLSS 5 was on).
namespace mfgunlock::nr::vk {
inline thread_local bool t_forwarding_game_call = false;
}
