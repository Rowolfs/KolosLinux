// Linux stream diagnostic: pool owns slot selection and presentation.
#include <kolos/diagnostic_renderer.h>
#include <kolos/buffer_pool.h>
#include <kolos_stream_wire.h>
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
        KolosBufferPool pool(v, path);
        DiagnosticRenderer renderer;
        auto started=std::chrono::steady_clock::now();
        for(uint32_t frame=0;frame<frames;frame++){
            auto buffer = pool.acquire();
            auto commands = pool.prepareForRender(buffer);
            renderer.record(commands, buffer.image(), frame);
            pool.prepareForPresent(buffer);
            pool.present(buffer, frame);
            if(frame==0||(frame+1)%60==0)std::printf("Presented %u/%u frames\n",frame+1,frames);
            // Conservative 30 FPS target; this test serializes GPU and presentation waits.
            std::this_thread::sleep_until(started+std::chrono::microseconds(33333LL*(frame+1)));
        }
        pool.stop();
        std::puts("PASS: triple-buffer stream completed; last displayed buffer released.");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"STOP: %s\n",e.what());return 1;}
}
