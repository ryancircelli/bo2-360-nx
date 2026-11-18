#include <rex/exception_handler.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void* page;
static bool Handle(rex::arch::Exception* ex, void*) {
  if (ex->fault_address() != reinterpret_cast<uint64_t>(page)) return false;
  return mprotect(page, getpagesize(), PROT_READ | PROT_WRITE) == 0;
}
static bool Decline(rex::arch::Exception*, void*) { return false; }
static void Prior(int) { _exit(42); }
int main() {
  for (int mode=0;mode<4;++mode) {
    pid_t pid=fork();
    if (!pid) {
      alarm(3);
      if (mode==2) { signal(SIGBUS, Prior); signal(SIGSEGV, Prior); }
      if (mode==3) { signal(SIGBUS, SIG_IGN); signal(SIGSEGV, SIG_IGN); }
      page=mmap(nullptr,getpagesize(),PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);
      rex::arch::ExceptionHandler::Install(mode==1 ? Handle : Decline,nullptr);
      *static_cast<volatile char*>(page)=1;
      _exit(mode==1 ? 0 : 99);
    }
    int status=0; waitpid(pid,&status,0);
    bool ok=mode==1 ? WIFEXITED(status)&&WEXITSTATUS(status)==0 : mode==2 ? WIFEXITED(status)&&WEXITSTATUS(status)==42 : WIFSIGNALED(status)&&(WTERMSIG(status)==SIGBUS||WTERMSIG(status)==SIGSEGV);
    printf("mode %d status %d %s\n",mode,status,ok?"PASS":"FAIL");
    if (!ok) return 1;
  }
}
