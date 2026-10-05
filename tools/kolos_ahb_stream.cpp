// FIFO scheduling stays here until KolosBufferPool.
#include <kolos/diagnostic_renderer.h>
#include <kolos_stream_wire.h>
#include <array>
#include <deque>
#include <thread>
#include <cstdio>
#include <cstdlib>
using namespace kolos;
static void need(bool ok, const char* text) { kw::require(ok, text); }
int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    try{
        const std::string path=argc>1?argv[1]:"/mnt/kolos-wayland/ahb-stream.sock";
        uint32_t frames=300;
        if(argc>2){char* end=nullptr;long n=strtol(argv[2],&end,10);need(end&&*end=='\0'&&n>0&&n<=100000,"invalid frame count");frames=uint32_t(n);}
        Context v;initialize(v);
        std::array<Imported,3> buffers;
        for(uint32_t i=0;i<3;i++)importBuffer(v,buffers[i],path,i);
        auto control=kw::connectTo(path);kw::send(control.value,kw::START);
        std::deque<uint32_t> free;
        std::array<bool,3> freeFlags{false,false,false};
        auto addFree=[&](const kw::Message& m){
            need(m.op==kw::FREE&&m.index<3&&!freeFlags[m.index],"invalid/duplicate FREE");
            freeFlags[m.index]=true;free.push_back(m.index);
        };
        for(int i=0;i<3;i++)addFree(kw::read(control.value));
        auto started=std::chrono::steady_clock::now();
        for(uint32_t frame=0;frame<frames;frame++){
            need(!free.empty(),"no released buffer");uint32_t index=free.front();free.pop_front();freeFlags[index]=false;
            draw(v,buffers[index],frame);
            kw::send(control.value,kw::PRESENT,index,frame);
            for(;;){
                auto m=kw::read(control.value);
                if(m.op==kw::FREE){addFree(m);continue;}
                need(m.op==kw::ACK&&m.index==index&&m.serial==frame,"invalid presentation ACK");break;
            }
            if(frame==0||(frame+1)%60==0)std::printf("Presented %u/%u frames, slot=%u\n",frame+1,frames,index);
            // Conservative 30 FPS target; this test serializes GPU and presentation waits.
            std::this_thread::sleep_until(started+std::chrono::microseconds(33333LL*(frame+1)));
        }
        kw::send(control.value,kw::STOP,0,frames);
        auto done=kw::read(control.value);need(done.op==kw::DONE&&done.serial==frames,"missing final release confirmation");
        std::puts("PASS: triple-buffer stream completed; last displayed buffer released.");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"STOP: %s\n",e.what());return 1;}
}
