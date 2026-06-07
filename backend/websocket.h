#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sys/socket.h>

namespace ws {

inline std::string base64_encode(const unsigned char* data, int len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, len);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

inline std::string compute_accept_key(const std::string& client_key) {
    std::string input = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)input.c_str(), input.size(), hash);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

inline std::string find_header(const std::string& request, const std::string& name) {
    std::string search = name + ": ";
    size_t pos = request.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = request.find("\r\n", pos);
    std::string val = request.substr(pos, end - pos);
    size_t start = val.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t last = val.find_last_not_of(" \t\r\n");
    return val.substr(start, last - start + 1);
}

struct Frame {
    bool fin;
    uint8_t opcode;
    std::string payload;
};

inline Frame decode_frame(int fd) {
    Frame f{};
    uint8_t header[2];
    if (recv(fd, header, 2, MSG_WAITALL) != 2) return f;

    f.fin = header[0] & 0x80;
    f.opcode = header[0] & 0x0F;
    bool masked = header[1] & 0x80;
    uint64_t len = header[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        recv(fd, ext, 2, MSG_WAITALL);
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        recv(fd, ext, 8, MSG_WAITALL);
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked) recv(fd, mask, 4, MSG_WAITALL);

    f.payload.resize(len);
    recv(fd, f.payload.data(), len, MSG_WAITALL);

    if (masked)
        for (uint64_t i = 0; i < len; i++)
            f.payload[i] ^= mask[i % 4];

    return f;
}

inline void send_frame(int fd, uint8_t opcode, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode);

    if (payload.size() < 126) {
        frame.push_back((uint8_t)payload.size());
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((payload.size() >> 8) & 0xFF);
        frame.push_back(payload.size() & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((payload.size() >> (i * 8)) & 0xFF);
    }

    frame.insert(frame.end(), payload.begin(), payload.end());
    send(fd, frame.data(), frame.size(), 0);
}

}
