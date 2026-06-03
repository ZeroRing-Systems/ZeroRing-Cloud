#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ws {

struct SHA1 {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t count = 0;
    uint8_t buffer[64]{};

    static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

    void transform(const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);       k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;                k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        size_t idx = count % 64;
        count += len;
        for (size_t i = 0; i < len; i++) {
            buffer[idx++] = data[i];
            if (idx == 64) { transform(buffer); idx = 0; }
        }
    }

    std::string digest() {
        uint64_t bits = count * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (count % 64 != 56) update(&pad, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t b = (bits >> (i * 8)) & 0xFF;
            update(&b, 1);
        }
        std::string out(20, '\0');
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (state[i] >> 24) & 0xFF;
            out[i*4+1] = (state[i] >> 16) & 0xFF;
            out[i*4+2] = (state[i] >> 8)  & 0xFF;
            out[i*4+3] =  state[i]        & 0xFF;
        }
        return out;
    }
};

inline std::string base64_encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string compute_accept_key(const std::string& client_key) {
    std::string concat = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    SHA1 sha;
    sha.update(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    return base64_encode(sha.digest());
}

inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::string find_header(const std::string& request, const std::string& name) {
    std::string search = name + ": ";
    auto pos = request.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = request.find("\r\n", pos);
    return trim(request.substr(pos, end - pos));
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
        frame.push_back(static_cast<uint8_t>(payload.size()));
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
