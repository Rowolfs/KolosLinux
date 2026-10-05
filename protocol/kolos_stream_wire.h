#pragma once
// Private, little-endian diagnostic protocol. Android arm64 <-> Debian arm64.
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <string>
namespace kw {
constexpr uint32_t MAGIC=0x3154534b; // KST1
constexpr uint32_t GET=1, START=2, FREE=3, PRESENT=4, ACK=5, STOP=6, DONE=7;
struct Message { uint32_t magic=MAGIC, op=0, index=0, serial=0; };
static_assert(sizeof(Message)==16);
struct Fd {
    int value=-1;
    explicit Fd(int v=-1):value(v){}
    ~Fd(){if(value>=0) close(value);}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value(other.value){other.value=-1;}
    Fd& operator=(Fd&& other) noexcept {
        if(this!=&other){if(value>=0)close(value);value=other.value;other.value=-1;}return *this;
    }
};
inline void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
inline void wait(int fd,short events,const std::atomic_bool* cancel=nullptr,int timeoutMs=30000){
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    for(;;){
        if(cancel && cancel->load())throw std::runtime_error("session cancelled");
        if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("I/O timeout");
        pollfd p{fd,events,0}; int n=poll(&p,1,100);
        if(n<0){if(errno==EINTR)continue;throw std::runtime_error("poll failed");}
        if(n && (p.revents&events))return;
        if(n && (p.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("peer/fence disconnected");
    }
}
inline void readAll(int fd,void* data,size_t size,const std::atomic_bool* cancel=nullptr){
    auto p=static_cast<unsigned char*>(data);
    while(size){
        wait(fd,POLLIN,cancel);
        ssize_t n=recv(fd,p,size,MSG_DONTWAIT);
        if(n<0 && (errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        require(n>0,"peer closed while reading"); p+=n;size-=size_t(n);
    }
}
inline void writeAll(int fd,const void* data,size_t size,const std::atomic_bool* cancel=nullptr){
    auto p=static_cast<const unsigned char*>(data);
    while(size){
        wait(fd,POLLOUT,cancel);
        ssize_t n=send(fd,p,size,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0 && (errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        require(n>0,"peer closed while writing");p+=n;size-=size_t(n);
    }
}
inline Message read(int fd,const std::atomic_bool* cancel=nullptr){
    Message m;readAll(fd,&m,sizeof(m),cancel);require(m.magic==MAGIC,"protocol magic mismatch");return m;
}
inline void send(int fd,uint32_t op,uint32_t index=0,uint32_t serial=0,const std::atomic_bool* cancel=nullptr){
    Message m{MAGIC,op,index,serial};writeAll(fd,&m,sizeof(m),cancel);
}
inline sockaddr_un address(const std::string& path){
    sockaddr_un a{};a.sun_family=AF_UNIX;
    require(!path.empty()&&path.front()=='/'&&path.size()<sizeof(a.sun_path),"invalid socket path");
    memcpy(a.sun_path,path.c_str(),path.size()+1);return a;
}
inline Fd connectTo(const std::string& path){
    Fd fd(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));require(fd.value>=0,"socket failed");
    auto a=address(path);
    if(connect(fd.value,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){
        if(errno!=EINPROGRESS)throw std::runtime_error(std::string("connect: ")+strerror(errno));
        wait(fd.value,POLLOUT);int err=0;socklen_t len=sizeof(err);
        require(getsockopt(fd.value,SOL_SOCKET,SO_ERROR,&err,&len)==0&&err==0,"connect failed");
    }return fd;
}
}
