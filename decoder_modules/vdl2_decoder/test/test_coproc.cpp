// test_coproc.cpp - standalone test of the module's coprocess plumbing.
//
// Drives a REAL dumpvdl2 binary through the same fork/exec/pipe path the SDR++
// module uses: feeds a synthetic VDL2 burst (S16_LE I/Q) to dumpvdl2's stdin,
// reads its text output, splits it into per-message blocks, and checks the
// decode. Also verifies the "binary not found" status path. No SDR++ needed.
//
// Build & run:
//   1. Build dumpvdl2 (see ../README.md "Requirements").
//   2. Generate test IQ at /tmp/vdl2_test.s16 (S16_LE, 105000 sps, interleaved
//      I/Q) carrying a known VDL2/ACARS burst.
//   3. g++ -std=c++17 -O2 test/test_coproc.cpp -o /tmp/test_coproc -lpthread
//      /tmp/test_coproc /path/to/dumpvdl2
//   Expect: "*** COPROCESS PLUMBING OK ***"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>

struct Coproc {
    std::atomic<int> childStdin{-1};
    int childStdout=-1; pid_t childPid=-1;
    std::thread reader; std::atomic<bool> running{false};
    std::string status="idle", curBlock;
    std::mutex mtx; std::deque<std::string> blocks;

    void push(const std::string& b){ std::lock_guard<std::mutex> l(mtx); blocks.push_back(b); }

    bool start(const std::string& path, const char* freq){
        int in[2],out[2]; if(pipe(in)||pipe(out)){status="pipe fail";return false;}
        pid_t pid=fork(); if(pid<0){status="fork fail";return false;}
        if(pid==0){
            dup2(in[0],0); dup2(out[1],1);
            close(in[0]);close(in[1]);close(out[0]);close(out[1]);
            execlp(path.c_str(),path.c_str(),"--iq-file","-","--sample-format","S16_LE",
                   "--oversample","1","--centerfreq",freq,freq,
                   "--output","decoded:text:file:path=-",(char*)NULL);
            _exit(127);
        }
        close(in[0]); close(out[1]);
        childPid=pid; childStdout=out[0]; childStdin.store(in[1]); running.store(true);
        reader=std::thread([this]{ readerLoop(); });
        return true;
    }
    void readerLoop(){
        char buf[4096]; std::string acc;
        while(running.load()){
            ssize_t n=read(childStdout,buf,sizeof(buf));
            if(n>0){ acc.append(buf,n); size_t nl;
                while((nl=acc.find('\n'))!=std::string::npos){
                    std::string line=acc.substr(0,nl); acc.erase(0,nl+1);
                    bool hdr=(!line.empty()&&line[0]=='['&&line.find("dBFS")!=std::string::npos);
                    if(hdr&&!curBlock.empty()){ push(curBlock); curBlock.clear(); }
                    if(!curBlock.empty()) curBlock+="\n"; curBlock+=line;
                }
            } else if(n==0){ break; } else { if(errno==EINTR)continue; break; }
        }
        if(!curBlock.empty()){ push(curBlock); curBlock.clear(); }
        if(childPid>0){ int st=0; if(waitpid(childPid,&st,WNOHANG)==childPid){
            childPid=-1; if(WIFEXITED(st)&&WEXITSTATUS(st)==127) status="not found";
            else if(!running.load()) status="stopped"; else status="exited"; } }
        running.store(false);
    }
    void feed(const char* file){
        FILE* f=fopen(file,"rb"); if(!f)return; char b[8192]; size_t r;
        int fd=childStdin.load();
        while((r=fread(b,1,sizeof(b),f))>0){ size_t off=0;
            while(off<r){ ssize_t w=write(fd,b+off,r-off); if(w>0){off+=w;continue;}
                if(w<0&&errno==EINTR)continue; break; } }
        fclose(f);
    }
    void stop(){
        running.store(false); int s=childStdin.exchange(-1); if(s>=0)close(s);
        if(childPid>0)kill(childPid,SIGTERM);
        if(reader.joinable())reader.join();
        if(childStdout>=0){close(childStdout);childStdout=-1;}
        if(childPid>0){int st;waitpid(childPid,&st,0);childPid=-1;}
    }
};

int main(int argc,char**argv){
    signal(SIGPIPE,SIG_IGN);
    std::string dv = argc>1?argv[1]:"dumpvdl2";
    bool all=true;

    // Test A: real dumpvdl2, feed synthetic IQ, expect decoded blocks.
    { Coproc c; c.start(dv,"136975000"); c.feed("/tmp/vdl2_test.s16");
      // close stdin -> dumpvdl2 hits EOF and flushes; give it a moment
      int s=c.childStdin.exchange(-1); if(s>=0)close(s);
      for(int i=0;i<50 && c.running.load();i++) usleep(100000);
      c.stop();
      size_t n=c.blocks.size(); bool found=false;
      for(auto&b:c.blocks) if(b.find("HELLO VDL2 WORLD")!=std::string::npos) found=true;
      printf("[A real dumpvdl2] blocks=%zu  contains_msg=%d -> %s\n",n,found,
             (n>=1&&found)?"OK":"FAIL"); all=all&&(n>=1&&found);
    }
    // Test B: binary not found -> status "not found".
    { Coproc c; c.start("/nonexistent/dumpvdl2_xyz","136975000");
      for(int i=0;i<30 && c.running.load();i++) usleep(50000);
      c.stop();
      printf("[B not-found]    status='%s' -> %s\n",c.status.c_str(),
             c.status=="not found"?"OK":"FAIL"); all=all&&(c.status=="not found");
    }
    printf("\n%s\n", all?"*** COPROCESS PLUMBING OK ***":"*** FAIL ***");
    return all?0:1;
}
