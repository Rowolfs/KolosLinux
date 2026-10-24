#include "graphics_wire.h"
#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace kolos::graphics::wire {
void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (unsigned i=0;i<4;i++) out.push_back(uint8_t(v>>(8*i)));
}
void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (unsigned i=0;i<8;i++) out.push_back(uint8_t(v>>(8*i)));
}
uint32_t Reader::get32() {
    if (bytes_.size()-at_<4) throw std::runtime_error("short graphics payload");
    uint32_t v=0; for(unsigned i=0;i<4;i++) v|=uint32_t(bytes_[at_++])<<(8*i); return v;
}
uint64_t Reader::get64() {
    if (bytes_.size()-at_<8) throw std::runtime_error("short graphics payload");
    uint64_t v=0; for(unsigned i=0;i<8;i++) v|=uint64_t(bytes_[at_++])<<(8*i); return v;
}
void Reader::end() const { if(at_!=bytes_.size()) throw std::runtime_error("extra graphics payload"); }
static void ready(int fd, short events) {
    pollfd p{fd,events,0};
    for(;;) {
        int n=poll(&p,1,30000);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw std::runtime_error("graphics socket timeout/error");
        if(p.revents&events) return;
        throw std::runtime_error("graphics socket disconnected");
    }
}
static void readAll(int fd,uint8_t* out,size_t count) {
    while(count) {
        ready(fd,POLLIN);
        ssize_t n=recv(fd,out,count,MSG_DONTWAIT);
        if(n<0 && (errno==EINTR||errno==EAGAIN)) continue;
        if(n<=0) throw std::runtime_error("graphics socket EOF");
        out+=n; count-=size_t(n);
    }
}
static void writeAll(int fd,const uint8_t* data,size_t count) {
    while(count) {
        ready(fd,POLLOUT);
        ssize_t n=send(fd,data,count,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0 && (errno==EINTR||errno==EAGAIN)) continue;
        if(n<=0) throw std::runtime_error("graphics socket write failed");
        data+=n; count-=size_t(n);
    }
}
Frame readFrame(int fd) {
    uint8_t header[16]; readAll(fd,header,sizeof(header));
    std::vector<uint8_t> h(header,header+16); Reader r(h);
    if(r.get32()!=kMagic) throw std::runtime_error("graphics magic mismatch");
    if(r.get32()!=kVersion) throw std::runtime_error("graphics protocol version mismatch");
    Frame f{r.get32(),{}}; uint32_t length=r.get32();
    if(length>kMaxPayload) throw std::runtime_error("graphics payload too large");
    f.payload.resize(length);
    if(length) readAll(fd,f.payload.data(),length);
    return f;
}
void writeFrame(int fd,uint32_t opcode,const std::vector<uint8_t>& payload) {
    if(payload.size()>kMaxPayload) throw std::runtime_error("graphics payload too large");
    std::vector<uint8_t> bytes; bytes.reserve(16+payload.size());
    put32(bytes,kMagic);put32(bytes,kVersion);put32(bytes,opcode);put32(bytes,uint32_t(payload.size()));
    bytes.insert(bytes.end(),payload.begin(),payload.end());
    writeAll(fd,bytes.data(),bytes.size());
}
} // namespace kolos::graphics::wire
