#include <cstdlib>
#include <iostream>
#include "../src/addons/mfgunlock/blackwell.hpp"
#include "../src/addons/mfgunlock/binary_patch.hpp"
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace bw=mfgunlock::blackwell;
// 1.10.1: the kernel replacements come from the local 1.4.5 inputs (blackwell_cubins.generated.hpp),
// which CI does not have; without them there is nothing to check.
#if !MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
int wmain(int,wchar_t**) {
  std::cout<<"SKIP: blackwell_cubins.generated.hpp is a local input\n";
  return 77;
}
#else
int wmain(int argc,wchar_t** argv) {
  if(argc<2){std::cout<<"SKIP: provide installed PE inputs\n";return 77;}
  size_t kernels=0,profiles=0;
  uint64_t previous_generation=0;
  for(int i=1;i<argc;++i){
    HMODULE module=LoadLibraryExW(argv[i],nullptr,DONT_RESOLVE_DLL_REFERENCES);CHECK(module);
    std::vector<bw::internal::Candidate> candidates;std::string why;
    if(bw::internal::CollectCandidates(module,candidates,why))for(const auto& candidate:candidates){
      ++kernels;std::vector<uint8_t> original(candidate.payload,candidate.payload+candidate.slot_size);
      bw::internal::ElfFingerprint fp{};CHECK(bw::internal::FingerprintElf(original.data(),original.size(),fp));
      CHECK(bw::internal::MatchReplacement(fp,original.size()));
      // Instruction, parameter/metadata, entrypoint and ABI mutations.
      const auto table=bw::internal::ReadU64(original.data()+0x28);
      const auto stride=bw::internal::ReadU16(original.data()+0x3a);
      const auto count=bw::internal::ReadU16(original.data()+0x3c);
      const auto strings=bw::internal::ReadU16(original.data()+0x3e);
      const auto names=bw::internal::ReadU64(original.data()+table+strings*stride+0x18);
      size_t tested=0;
      for(size_t j=0;j<count;++j){
        auto* section=original.data()+table+j*stride;
        const auto offset=bw::internal::ReadU64(section+0x18),size=bw::internal::ReadU64(section+0x20);
        const auto* name=reinterpret_cast<const char*>(original.data()+names+bw::internal::ReadU32(section));
        if(!size || offset>=original.size() || bw::internal::ReadU32(section+4)==8)continue;
        if(std::strncmp(name,".text.",6)!=0 && std::strncmp(name,".nv.info",8)!=0 && std::strcmp(name,".strtab")!=0)continue;
        auto mutated=original;mutated[offset]^=1;bw::internal::ElfFingerprint changed{};
        CHECK(!bw::internal::FingerprintElf(mutated.data(),mutated.size(),changed) ||
              !bw::internal::MatchReplacement(changed,mutated.size()));++tested;
      }
      CHECK(tested>=2);
      auto abi=original;abi[18]^=1;
      CHECK(!bw::internal::FingerprintElf(abi.data(),abi.size(),fp));
      CHECK(bw::internal::FingerprintElf(original.data(),original.size(),fp));
      CHECK(!bw::internal::MatchReplacement(fp,original.size()-8));
      auto forged=fp;forged.payload_hash=0;CHECK(!bw::internal::MatchReplacement(forged,original.size()));
    }
    const auto* session=mfgunlock::modulesession::Acquire(module);CHECK(session);
    CHECK(session->generation>previous_generation);previous_generation=session->generation;
    CHECK(!mfgunlock::modulesession::SameGeneration(*session,session->generation+1));
    for(uint32_t kind:{0u,2u,1u}){
      const auto* profile=mfgunlock::binarypatch::ProfileFor(module,kind);if(!profile)continue;
      ++profiles;std::vector<mfgunlock::patching::Write> writes;
      CHECK(mfgunlock::binarypatch::Plan(module,*profile,writes));
      if(kind==0 || kind==2) for(const auto& w:writes)CHECK(w.original.size()==1);
      CHECK(mfgunlock::binarypatch::Apply(module,writes));
      CHECK(mfgunlock::modulesession::Validate(*session));
      CHECK(mfgunlock::patching::Restore(writes,true));
      CHECK(mfgunlock::modulesession::Validate(*session));
      if(kind==2){
        writes.clear();CHECK(mfgunlock::binarypatch::Plan(module,*profile,writes,5));
        for(const auto& w:writes)CHECK(w.original.size()==1);
        CHECK(mfgunlock::binarypatch::Apply(module,writes));
        CHECK(*(reinterpret_cast<uint8_t*>(module)+profile->sites[0].rva+1)==5);
        CHECK(mfgunlock::modulesession::Validate(*session));
        CHECK(mfgunlock::patching::Restore(writes,true));
        CHECK(mfgunlock::modulesession::Validate(*session));
      }
      // Reject foreign code changes even with the same PE metadata and address.
      auto* at=reinterpret_cast<uint8_t*>(module)+profile->sites[0].rva+profile->sites[0].size+1;
      DWORD old=0,ignored=0;CHECK(VirtualProtect(at,1,PAGE_EXECUTE_READWRITE,&old));
      *at^=1;CHECK(!mfgunlock::modulesession::Validate(*session));
      CHECK(!mfgunlock::binarypatch::ProfileFor(module,kind));
      *at^=1;CHECK(VirtualProtect(at,1,old,&ignored));
      CHECK(mfgunlock::modulesession::Validate(*session));
    }
    FreeLibrary(module);
    // A live lease prevents address reuse while any patch or cached pointer can reference it.
    HMODULE retained=nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(module),&retained) && retained==module);
  }
  CHECK(kernels>=3 && profiles>=2);
  std::cout<<kernels<<" original kernels mutated; "<<profiles<<" exact binary profiles apply/restore; session leases passed\n";
}
#endif
