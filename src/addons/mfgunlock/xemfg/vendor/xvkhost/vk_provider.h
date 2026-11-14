// SPDX-License-Identifier: MIT
#pragma once
// MFGAdaUnlock 1.10.0: upstream's vk_provider.h declares XeMFG as OptiScaler's
// Vulkan DLSS-G provider. This build never runs under OptiScaler, so the host's
// one question to it (is the provider's shim installed?) is always no.
namespace xess_vulkan_provider {
inline bool ShimInstalled() noexcept { return false; }
}
