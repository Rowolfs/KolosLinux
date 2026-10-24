#include <kolos/buffer_pool.h>
#include <kolos/diagnostic_renderer.h>
#include <thread>
#include <chrono>
#include <cstdio>
#include <string>

using namespace kolos;
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if(argc!=2 && (argc!=4 || std::string(argv[2])!="--frames"))
            throw std::runtime_error("usage: kolos_graphics_smoke /run/kolos/graphics.sock [--frames N]");
        uint32_t frames=300;
        if(argc==4) {
            size_t consumed=0;
            unsigned long long requested=std::stoull(argv[3],&consumed);
            if(consumed!=std::string(argv[3]).size() || requested==0 || requested>1000000)
                throw std::runtime_error("--frames must be between 1 and 1000000");
            frames=static_cast<uint32_t>(requested);
        }
        Context context;initialize(context);
        KolosBufferPool pool(context,argv[1],KolosBufferPool::GraphicsV2{});
        DiagnosticRenderer renderer;
        auto start=std::chrono::steady_clock::now();
        uint32_t frame=0;
        while(frame<frames) {
            auto buffer=pool.acquire();
            VkCommandBuffer commands=pool.prepareForRender(buffer);
            renderer.record(commands,buffer.image(),frame);
            pool.prepareForPresent(buffer); // GPU fence wait before PRESENT.
            if(!pool.present(buffer,frame)) {
                std::printf("Generation changed; retrying frame %u\n",frame);
                continue;
            }
            ++frame;
            if(frame==1 || frame%60==0 || frame==frames)
                std::printf("Presented %u/%u\n",frame,frames);
            std::this_thread::sleep_until(start+std::chrono::microseconds(33333LL*frame));
        }
        pool.stop(); // Waits for final FREE and DONE.
        std::printf("PASS: %u/%u\n",frames,frames);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"STOP: %s\n",e.what());
        return 1;
    }
}
