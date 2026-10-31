// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>
#include <vector>

#include "../src/addons/mfgunlock/xemfg/synthetic_queue.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using mfgunlock::xemfg::native::synthetic::FrameQueue;

namespace {
struct Game {
  FrameQueue queue;
  uint32_t lastSlept{};
  std::vector<uint32_t> shown;
  void Slept(uint32_t id) { CHECK(id==lastSlept+1); lastSlept=id; } // XeLL: consecutive, once each
  uint32_t Evaluate() {
    const auto s=queue.Submit();
    if(s.start) Slept(s.id);
    queue.Commit(s.id);
    return s.id;
  }
  uint32_t Present() {
    const auto id=queue.BeforePresent();
    shown.push_back(id);
    const auto p=queue.AfterPresent();
    CHECK(p.id==id);
    if(p.start) Slept(p.start);
    return id;
  }
};
}

int main() {
  { // A plain game: one evaluation, one Present.
    Game g;
    for(uint32_t i=1;i<=50;++i) { CHECK(g.Evaluate()==i); CHECK(g.Present()==i); }
    CHECK(!g.queue.Pipelined() && g.queue.emptyPresents==0);
  }
  { // Two evaluations in every frame stay one frame, for good.
    Game g;
    for(uint32_t i=1;i<=50;++i) { CHECK(g.Evaluate()==i); CHECK(g.Evaluate()==i); CHECK(g.Present()==i); }
    CHECK(!g.queue.Pipelined() && g.queue.mergedEvaluations==50);
  }
  { // Menus: Presents without any evaluation change nothing.
    Game g;
    for(int i=0;i<20;++i) CHECK(g.Present()==0);
    CHECK(g.Evaluate()==1); CHECK(g.Present()==1);
    for(int i=0;i<20;++i) CHECK(g.Present()==0);
    CHECK(g.Evaluate()==2); CHECK(g.Present()==2);
    CHECK(!g.queue.Pipelined());
  }
  { // RDR2's order on D3D12: E E P P E E P P E P. Two proofs, then every Present has its frame.
    Game g;
    g.Evaluate(); g.Evaluate(); g.Present(); CHECK(g.Present()==0); CHECK(!g.queue.Pipelined());
    g.Evaluate(); g.Evaluate(); g.Present(); CHECK(g.Present()==0); CHECK(g.queue.Pipelined());
    CHECK(g.queue.TakeChanged() && !g.queue.TakeChanged());
    const auto empties=g.queue.emptyPresents;
    g.shown.clear();
    g.Evaluate(); g.Present();
    for(int cycle=0;cycle<40;++cycle) {
      g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
      g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
      g.Evaluate(); g.Present();
    }
    CHECK(g.queue.emptyPresents==empties);
    for(size_t i=1;i<g.shown.size();++i) CHECK(g.shown[i]==g.shown[i-1]+1); // in order, none lost
  }
  { // Steady lag: each evaluation before the previous frame's Present.
    Game g;
    g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
    g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
    CHECK(g.queue.Pipelined());
    g.Evaluate(); g.shown.clear();
    for(int i=0;i<40;++i) { g.Evaluate(); CHECK(g.Present()!=0); }
    for(size_t i=1;i<g.shown.size();++i) CHECK(g.shown[i]==g.shown[i-1]+1);
  }
  { // Wrongly proven: a queue that keeps growing merges again, for good.
    Game g;
    g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
    g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
    CHECK(g.queue.Pipelined()); g.queue.TakeChanged();
    uint32_t last{};
    for(uint32_t i=0;i<FrameQueue::kDepth+1;++i) last=g.Evaluate();
    CHECK(!g.queue.Pipelined() && g.queue.Refused() && g.queue.TakeChanged());
    CHECK(g.Present()==last); // the newest frame; the unshown ones are dropped
    CHECK(g.Evaluate()==last+1); CHECK(g.Evaluate()==last+1); CHECK(g.Present()==last+1);
    g.Present(); g.Evaluate(); g.Evaluate(); g.Present(); g.Present();
    CHECK(!g.queue.Pipelined()); // never proven again
  }
  { // An evaluation during a Present: the Present's frame finishes first.
    FrameQueue q;
    auto s=q.Submit(); q.Commit(s.id);
    CHECK(q.BeforePresent()==1);
    s=q.Submit(); CHECK(s.id==1 && !s.start); // merged: not proven yet
    const auto p=q.AfterPresent(); CHECK(p.id==1 && p.start==2);
  }
  { // A frame reserved but not committed (its sleep runs unlocked) opens nothing new.
    FrameQueue q;
    auto s=q.Submit(); q.Commit(s.id);
    q.BeforePresent(); q.AfterPresent();   // opens 2
    s=q.Submit(); q.Commit(s.id); CHECK(s.id==2);
    CHECK(q.BeforePresent()==2); CHECK(q.BeforePresent()==2);
    q.AfterPresent();
  }
  std::cout<<"PASS: synthesized frame order\n";
  return 0;
}
