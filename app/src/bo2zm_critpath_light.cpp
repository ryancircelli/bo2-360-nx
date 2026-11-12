#include "bo2zm_critpath_light.h"
#include "bo2zm_jobs.h"
#include <rex/cvar.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

REXCVAR_DECLARE(std::string, bo2zm_perf_log);
extern "C" { bool bo2zm_critpath_light = false; }
namespace bo2zm::critpath::light {
namespace {
namespace jobs = bo2zm::jobs;
uint64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr unsigned kGroups=4096, kTypes=32, kThreads=32, kCompletions=8, kRows=65536;
struct Type { std::atomic<uint32_t> descriptor{0}, function{0}; std::atomic<int32_t> pending{0}; };
struct Group { std::atomic<uint32_t> key{0}; std::array<Type,kTypes> types; };
std::array<Group,kGroups> groups;
std::atomic<uint64_t> group_drops{0}, type_drops{0}, underflows{0}, thread_drops{0}, wait_drops{0}, frame_drops{0};
Group* FindGroup(uint32_t key) {
  if (!key) return nullptr;
  auto hash = (key >> 4) ^ (key >> 14);
  for (unsigned i=0; i<16; ++i) {
    auto& g=groups[(hash+i)&(kGroups-1)];
    auto old=g.key.load(std::memory_order_relaxed);
    if (old==key) return &g;
    if (!old && g.key.compare_exchange_strong(old,key,std::memory_order_relaxed)) return &g;
    if (old==key) return &g;
  }
  group_drops.fetch_add(1,std::memory_order_relaxed); return nullptr;
}
int FindType(Group& g, uint32_t descriptor, const uint8_t* base) {
  for (unsigned i=0; i<kTypes; ++i) {
    auto& t=g.types[i]; auto old=t.descriptor.load(std::memory_order_relaxed);
    if (old==descriptor) return int(i);
    if (!old && t.descriptor.compare_exchange_strong(old,descriptor,std::memory_order_relaxed)) {
      t.function.store(jobs::Read32(base,descriptor+8),std::memory_order_relaxed); return int(i);
    }
    if (old==descriptor) return int(i);
  }
  type_drops.fetch_add(1,std::memory_order_relaxed); return -1;
}
struct Completion {
  std::atomic<uint64_t> seq{0}, ns{0};
  std::atomic<uint32_t> group{0}, type{0}, function{0}, record{0}, data{0};
};
struct Thread {
  char name[64]{}; uint32_t host=0; std::atomic<bool> ready{false};
  uint64_t written=0; // owned by this producer only
  std::array<Completion,kCompletions> completions;
  std::atomic<uint64_t> generation{0};
  std::atomic<uint32_t> completed_types{0};
};
std::array<Thread,kThreads> threads;
std::atomic<uint32_t> thread_count{0};
std::atomic<uint32_t> active_group{0}, watched_group{0};
thread_local bool tracing_wait=false;
std::atomic<uint64_t> generation{0};
Thread* ThisThread() {
  static thread_local Thread* own=[]() -> Thread* {
    auto i=thread_count.fetch_add(1,std::memory_order_relaxed);
    if (i>=kThreads) { thread_drops.fetch_add(1,std::memory_order_relaxed); return nullptr; }
    auto& t=threads[i];
    auto* x=rex::system::XThread::GetCurrentThread();
    auto* actual=x && x->thread() ? x->thread() : rex::thread::Thread::GetCurrentThread();
    if (actual) { const auto& n=actual->name(); std::snprintf(t.name,sizeof(t.name),"%.*s",int(n.size()),n.data()); t.host=actual->system_id(); }
    t.ready.store(true,std::memory_order_release);
    return &t;
  }();
  return own;
}
struct TypeRow { uint32_t descriptor=0, function=0; int32_t before=0, after=0; uint32_t completed=0; };
struct Wait {
  uint64_t start=0,end=0,last_ns=0;
  uint64_t sleep_warned=0,sleep_timeout=0,sleep_direct=0,sleep_ns=0,last_sleep_start=0,last_sleep_end=0,last_sleep_warned=0;
  uint32_t group=0, queued_start=0,running_start=0,queued_end=0,running_end=0;
  uint32_t last_type=0,last_function=0,last_record=0,last_data=0,last_thread=0;
  std::array<TypeRow,kTypes> types;
};
struct Frame { uint64_t frame=0,present=0; };
// Append-only bounded rings: never overwrite a published row. Dumping on TERM can safely read immutable rows
// while producers finish their current operation. Capacities cover 18 minutes at 60 presents/waits per second.
std::array<Wait,kRows> waits;
std::array<Frame,kRows> frames;
std::atomic<uint32_t> wait_count{0},frame_count{0};
Wait current; Group* current_group=nullptr; uint64_t current_generation=0; // Main only
std::filesystem::path out;
std::atomic<bool> stopping{false};
std::thread exit_writer;
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> term_requested{false};
using Handler=void(*)(int);
Handler previous_term=SIG_DFL;
void Term(int) { term_requested.store(true,std::memory_order_relaxed); }
void PrintTypes(std::FILE* f,const Wait& w) {
  bool first=true;
  for (const auto& t:w.types) if (t.descriptor && (t.before || t.after || t.completed)) {
    std::fprintf(f,"%s%08X:%08X:%d:%d:%u",first?"":";",t.descriptor,t.function,t.before,t.after,t.completed); first=false;
  }
  if (first) std::fputc('-',f);
}
void Dump() {
  auto* f=std::fopen((out/"critpath_light_frames.tsv").c_str(),"wb");
  auto* wf=std::fopen((out/"critpath_light_waits.tsv").c_str(),"wb");
  if (!f || !wf) { if(f)std::fclose(f); if(wf)std::fclose(wf); return; }
  const char* cols="group\tqueued_start\trunning_start\tqueued_end\trunning_end\tlast_finish_ns\tlast_type\tlast_function\tlast_record\tlast_data\tlast_thread\ttypes\tgroup_sleep_warned\tgroup_sleep_timeout\tgroup_sleep_direct\tgroup_sleep_ns\tlast_sleep_start\tlast_sleep_end\tlast_sleep_warned\n";
  std::fprintf(f,"# LIGHT mode; monotonic ns; types=descriptor:function:outstanding_start:outstanding_end:completed_during_wait; last_finish is callback return before scheduler counter release; last_thread indexes footer\nframe\tt_present\tmain_wait_ns\tmain_wait_calls\tlongest_start\tlongest_end\t"); std::fputs(cols,f);
  std::fprintf(wf,"start\tend\t");std::fputs(cols,wf);
  auto print=[&](std::FILE* file,const Wait& w) {
    std::fprintf(file,"%08X\t%u\t%u\t%u\t%u\t%llu\t%08X\t%08X\t%08X\t%08X\t%u\t",w.group,w.queued_start,w.running_start,w.queued_end,w.running_end,(unsigned long long)w.last_ns,w.last_type,w.last_function,w.last_record,w.last_data,w.last_thread); PrintTypes(file,w);
    std::fprintf(file,"\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",(unsigned long long)w.sleep_warned,(unsigned long long)w.sleep_timeout,(unsigned long long)w.sleep_direct,(unsigned long long)w.sleep_ns,(unsigned long long)w.last_sleep_start,(unsigned long long)w.last_sleep_end,(unsigned long long)w.last_sleep_warned);
  };
  auto nw=wait_count.load(std::memory_order_acquire),nf=frame_count.load(std::memory_order_acquire);
  for(unsigned i=0;i<nw;++i) { const auto& w=waits[i];std::fprintf(wf,"%llu\t%llu\t",(unsigned long long)w.start,(unsigned long long)w.end);print(wf,w); }
  unsigned first=0; uint64_t previous=0;
  for(unsigned i=0;i<nf;++i) {
    const auto& fr=frames[i]; while(first<nw && waits[first].end<=previous) ++first;
    uint64_t total=0,calls=0,longest=0; const Wait* best=nullptr;
    for(unsigned j=first;j<nw && waits[j].start<=fr.present;++j) {
      const auto& w=waits[j]; if(w.start>previous) ++calls;
      const auto lo=std::max(w.start,previous),hi=std::min(w.end,fr.present); if(hi>lo)total+=hi-lo;
      if(w.end-w.start>longest) { longest=w.end-w.start;best=&w; }
    }
    static const Wait empty;
    const auto& w=best?*best:empty;
    std::fprintf(f,"%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t",(unsigned long long)fr.frame,(unsigned long long)fr.present,(unsigned long long)total,(unsigned long long)calls,(unsigned long long)w.start,(unsigned long long)w.end);print(f,w);
    previous=fr.present;
  }
  for(auto* file:{f,wf}) {
    for(unsigned i=0;i<std::min(thread_count.load(),kThreads);++i) if(threads[i].ready.load(std::memory_order_acquire)) std::fprintf(file,"#thread\t%u\t%u\t%s\n",i,threads[i].host,threads[i].name);
    std::fprintf(file,"#drops\tgroups=%llu\ttypes=%llu\tthreads=%llu\twaits=%llu\tframes=%llu\tunderflows=%llu\n",(unsigned long long)group_drops.load(),(unsigned long long)type_drops.load(),(unsigned long long)thread_drops.load(),(unsigned long long)wait_drops.load(),(unsigned long long)frame_drops.load(),(unsigned long long)underflows.load()); std::fclose(file);
  }
}
}
void Install() {
  bo2zm_critpath_light=true;
  auto p=std::filesystem::path(REXCVAR_GET(bo2zm_perf_log)); out=p.empty()?std::filesystem::current_path():p.parent_path();
  previous_term=std::signal(SIGTERM,Term);
  // The route harness ends with TERM, not normal C++ shutdown. The handler only sets a lock-free flag;
  // this sleeping thread does the exit-time I/O, then exits with the usual successful harness status.
  exit_writer=std::thread([] {
    while(!stopping.load(std::memory_order_relaxed) && !term_requested) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Dump();
    if(term_requested) std::_Exit(0);
  });
}
void Queued(const uint8_t* base,uint32_t record) {
  auto descriptor=jobs::Read32(base,record+jobs::kJobType); if(!descriptor)return;
  auto group=jobs::Read32(base,record+jobs::kJobGroup);
  // The filter may be an explicit group or the descriptor's own queued/running pair (+16).
  for(auto key:{group,group==descriptor+16?0u:descriptor+16}) {
    if(!key)continue;
    auto* g=FindGroup(key);if(!g)continue;int i=FindType(*g,descriptor,base);
    if(i>=0)g->types[i].pending.fetch_add(1,std::memory_order_relaxed);
  }
}
void Complete(const uint8_t* base,uint32_t record) {
  auto descriptor=jobs::Read32(base,record+jobs::kJobType); if(!descriptor)return;
  auto group=jobs::Read32(base,record+jobs::kJobGroup);
  auto active=active_group.load(std::memory_order_relaxed);
  Group* matched=nullptr;int matched_index=-1;
  for(auto key:{group,group==descriptor+16?0u:descriptor+16}) {
    if(!key)continue;
    auto* g=FindGroup(key);if(!g)continue;int i=FindType(*g,descriptor,base);if(i<0)continue;
    if(g->types[i].pending.fetch_sub(1,std::memory_order_relaxed)<=0)underflows.fetch_add(1,std::memory_order_relaxed);
    if(key==active) { matched=g;matched_index=i; }
  }
  if(!matched)return;
  auto gen=generation.load(std::memory_order_relaxed);auto* t=ThisThread();if(!t)return;
  auto ns=Now();
  if(t->generation.load(std::memory_order_relaxed)!=gen) { t->completed_types.store(0,std::memory_order_relaxed);t->generation.store(gen,std::memory_order_relaxed); }
  t->completed_types.fetch_or(1u<<matched_index,std::memory_order_relaxed);
  auto& c=t->completions[t->written++&(kCompletions-1)];
  c.seq.fetch_add(1,std::memory_order_acq_rel);
  c.group.store(active,std::memory_order_relaxed);c.type.store(descriptor,std::memory_order_relaxed);c.function.store(matched->types[matched_index].function.load(),std::memory_order_relaxed);
  c.record.store(record,std::memory_order_relaxed);c.data.store(jobs::Read32(base,record),std::memory_order_relaxed);c.ns.store(ns,std::memory_order_relaxed);
  c.seq.fetch_add(1,std::memory_order_release);
}
void Begin(const uint8_t* base,uint32_t group) {
  tracing_wait=true;watched_group.store(group,std::memory_order_relaxed);
  current=Wait{};current.group=group;current_group=FindGroup(group);
  current.queued_start=jobs::Read32(base,group);current.running_start=jobs::Read32(base,group+4);
  if(current_group)for(unsigned i=0;i<kTypes;++i) {
    auto& t=current_group->types[i];auto& r=current.types[i];r.descriptor=t.descriptor.load();r.function=t.function.load();r.before=t.pending.load();
  }
  current_generation=generation.fetch_add(1,std::memory_order_relaxed)+1;
  current.start=Now();active_group.store(group,std::memory_order_release);
}
void End(const uint8_t* base) {
  current.end=Now();tracing_wait=false;active_group.store(0,std::memory_order_release);
  current.queued_end=jobs::Read32(base,current.group);current.running_end=jobs::Read32(base,current.group+4);
  uint32_t mask=0;
  for(unsigned ti=0;ti<std::min(thread_count.load(),kThreads);++ti) {
    auto& t=threads[ti]; if(t.generation.load()==current_generation)mask|=t.completed_types.load();
    for(auto& c:t.completions) {
      auto seq=c.seq.load(std::memory_order_acquire); if(seq&1)continue;
      auto ns=c.ns.load(std::memory_order_relaxed);auto g=c.group.load(std::memory_order_relaxed);
      auto type=c.type.load(std::memory_order_relaxed),fn=c.function.load(std::memory_order_relaxed),record=c.record.load(std::memory_order_relaxed),data=c.data.load(std::memory_order_relaxed);
      if(seq!=c.seq.load(std::memory_order_acquire))continue;
      if(g==current.group && ns>=current.start && ns<=current.end && ns>current.last_ns) { current.last_ns=ns;current.last_type=type;current.last_function=fn;current.last_record=record;current.last_data=data;current.last_thread=ti; }
    }
  }
  if(current_group)for(unsigned i=0;i<kTypes;++i) {
    auto& t=current_group->types[i];auto& r=current.types[i];r.descriptor=t.descriptor.load();r.function=t.function.load();r.after=t.pending.load();r.completed=(mask>>i)&1;
  }
  auto n=wait_count.load(std::memory_order_relaxed);
  if(n<kRows) { waits[n]=current;wait_count.store(n+1,std::memory_order_release); }else wait_drops.fetch_add(1,std::memory_order_relaxed);
}
void Present(uint64_t frame) {
  auto n=frame_count.load(std::memory_order_relaxed);
  if(n<kRows) { frames[n]={frame,Now()};frame_count.store(n+1,std::memory_order_release); }else frame_drops.fetch_add(1,std::memory_order_relaxed);
}
uint32_t WatchedGroup() { return watched_group.load(std::memory_order_relaxed); }
void GroupSleep(bool warned,bool direct,uint64_t start,uint64_t end) {
  if(!tracing_wait)return;
  if(warned)++current.sleep_warned;else ++current.sleep_timeout;
  if(direct)++current.sleep_direct;
  current.sleep_ns+=end-start;current.last_sleep_start=start;current.last_sleep_end=end;current.last_sleep_warned=warned;
}
void Close() {
  stopping.store(true,std::memory_order_relaxed);
  if(exit_writer.joinable())exit_writer.join();
  std::signal(SIGTERM,previous_term);
}
}
extern "C" void Bo2LightJobComplete(const uint8_t* base,uint32_t record) { bo2zm::critpath::light::Complete(base,record); }
