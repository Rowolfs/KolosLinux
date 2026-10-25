#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace kolos::graphics::wire {
constexpr uint32_t kMagic = 0x3158474b; // KGX1, little endian
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxPayload = 4096;
enum Opcode : uint32_t {
    HELLO=1, HELLO_ACK=2, SESSION_INFO=3, GET_BUFFER=4, BUFFER_INFO=5,
    IMPORT_ACK=6, START=7, FREE=8, PRESENT=9, ACK=10, STOP=11,
    DONE=12, ERROR=13, POOL_REPLACED=14, POOL_READY=15,
    PRESENT_FENCED=16
};
struct Frame { uint32_t opcode; std::vector<uint8_t> payload; };
void put32(std::vector<uint8_t>& out, uint32_t value);
void put64(std::vector<uint8_t>& out, uint64_t value);
class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    uint32_t get32();
    uint64_t get64();
    void end() const;
private:
    const std::vector<uint8_t>& bytes_;
    size_t at_ = 0;
};
Frame readFrame(int fd);
void writeFrame(int fd, uint32_t opcode, const std::vector<uint8_t>& payload = {});
void writeFrameWithFd(int fd, uint32_t opcode, const std::vector<uint8_t>& payload, int sendFd);
} // namespace kolos::graphics::wire
