// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace mfgunlock::xemfg::native::synthetic {
// 1.7.1: which source frame an adapter's evaluation and a Present belong to.
// One frame id is open at a time and takes the evaluation; Presents consume ids
// in order. Two evaluations before a Present are ambiguous: one frame evaluated
// twice, or the next frame's evaluation ahead of this frame's Present (RDR2 on
// D3D12). The start assumes the first. A later Present with no frame proves the
// second: after kEvidence of those, each evaluation gets its own id. A queue that
// still grows was the first case after all, and the merge comes back for good.
// Callers hold the adapter's lock. Ids stay consecutive: XeLL requires it.
class FrameQueue {
 public:
  static constexpr uint32_t kDepth=4,kEvidence=2;
  struct Submission { uint32_t id; bool start; }; // start: the id needs its sleep first
  struct Presented { uint32_t id,start; };        // start: the next id to open, or 0
  Submission Submit() {
    ++evaluations;
    if(!building) { building=1; return {building,true}; }
    if(!buildingSubmitted) return {building,false};
    if(!pipelined) { merged=true; ++mergedEvaluations; return {building,false}; }
    if(count>=kDepth) {
      pipelined=false; refused=true; changed=true; merged=true; ++mergedEvaluations;
      pending[0]=building; count=1;
      return {building,false};
    }
    ++building; buildingSubmitted=false;
    return {building,true};
  }
  // The evaluation's frame is ready for its Present (its sleep is over).
  void Commit(uint32_t id) {
    if(buildingSubmitted || id!=building) return;
    pending[count++]=id; buildingSubmitted=true;
  }
  // The frame this Present shows, or 0.
  uint32_t BeforePresent() {
    if(count) { lastMerged=merged; merged=false; return presenting=pending[0]; }
    ++emptyPresents;
    if(lastMerged) {
      lastMerged=false;
      if(!refused && !pipelined && ++evidence>=kEvidence) { pipelined=true; changed=true; }
    }
    return presenting=0;
  }
  Presented AfterPresent() {
    if(!presenting || !count || pending[0]!=presenting) return {0,0};
    const auto id=presenting; presenting=0;
    for(uint32_t i=1;i<count;++i) pending[i-1]=pending[i];
    --count;
    if(!buildingSubmitted) return {id,0};
    ++building; buildingSubmitted=false;
    return {id,building};
  }
  bool Pipelined() const { return pipelined; }
  bool Refused() const { return refused; }
  // True once after the mode changed, for the log.
  bool TakeChanged() { const bool value=changed; changed=false; return value; }
  uint64_t evaluations{},mergedEvaluations{},emptyPresents{};
 private:
  uint32_t pending[kDepth]{};
  uint32_t count{},building{},presenting{},evidence{};
  bool buildingSubmitted{},merged{},lastMerged{},pipelined{},refused{},changed{};
};
}
