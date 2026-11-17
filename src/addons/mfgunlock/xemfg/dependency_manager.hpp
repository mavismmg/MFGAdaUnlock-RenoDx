// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
namespace mfgunlock::dependencies {
enum class Health { Missing, Valid, UpdateOptional, Unknown, Error };
struct File { std::string name,path,version,sha256; Health health{Health::Missing}; bool mapped{},required{}; std::string role; };
struct Status { bool checked{},busy{},helperRunning{}; std::string message; std::vector<File> files; std::string installRoot; bool scanComplete{true}; };
void Inspect();
void Repair();
Status Snapshot();
void Draw();
// 1.10.1 (plan item 8): the MFGUnlock-Streamline files for Add frame generation.
struct StreamlineStatus { bool checked{},helperRunning{}; int missing{},different{}; std::string message; };
void InspectStreamline();
void RepairStreamline();
StreamlineStatus StreamlineSnapshot();
}
