#include <kolos/buffer_pool.h>
#include <kolos/diagnostic_renderer.h>
#include <thread>
#include <chrono>
#include <cstdio>

using namespace kolos;
int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if(argc!=2) throw std::runtime_error("usage: kolos_graphics_smoke /run/kolos/graphics.sock");
        Context context;initialize(context);
        KolosBufferPool pool(context,argv[1],KolosBufferPool::GraphicsV1{});
        DiagnosticRenderer renderer;
        auto start=std::chrono::steady_clock::now();
        for(uint32_t frame=0;frame<300;frame++) {
            auto buffer=pool.acquire();
            VkCommandBuffer commands=pool.prepareForRender(buffer);
            renderer.record(commands,buffer.image(),frame);
            pool.prepareForPresent(buffer); // GPU fence wait before PRESENT.
            pool.present(buffer,frame);
            if(frame==0 || (frame+1)%60==0)
                std::printf("Presented %u/300\n",frame+1);
            std::this_thread::sleep_until(start+std::chrono::microseconds(33333LL*(frame+1)));
        }
        pool.stop(); // Waits for final FREE and DONE.
        std::puts("PASS: 300/300");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"STOP: %s\n",e.what());
        return 1;
    }
}
