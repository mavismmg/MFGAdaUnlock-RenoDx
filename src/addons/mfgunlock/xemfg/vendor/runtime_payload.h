#pragma once
#include <Windows.h>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace xess_payload {
// Only the portable DXIL programs are embedded. Intel's runtime DLLs are never
// shipped; they come from the game or from the user (see xess_runtime.h).
struct Asset { unsigned id; const char* name; const char* hash; size_t bytes; };
std::span<const Asset> Assets() noexcept;
std::span<const unsigned char> Read(HMODULE,const Asset&);
using Programs=std::map<std::string,std::vector<unsigned char>>;
Programs LoadPrograms(HMODULE);
std::string Hash(std::span<const unsigned char> bytes);
// RTXMFG's own shaders (RCDATA 7100.., xess/generate_xess_shaders.py). Each replaces exactly
// one Intel program, identified by the SHA-256 and size of the provider's decoded bytes; it was proven
// to produce byte-identical results against those bytes (and carries the same root signature part).
struct Replacement { std::string name, nativeHash; size_t nativeBytes{}; std::vector<unsigned char> bytes; };
std::span<const Asset> ShaderAssets() noexcept;
std::vector<Replacement> LoadReplacements(HMODULE);
}
