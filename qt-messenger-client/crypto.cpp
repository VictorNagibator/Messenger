#include "crypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <vector>
#include <stdexcept>

std::string encrypt(const std::string& plaintext, const std::string& key) {
    if (key.size()!=32) throw std::runtime_error("Key must be 32 bytes");
    std::vector<unsigned char> iv(16);
    if (!RAND_bytes(iv.data(), iv.size())) throw std::runtime_error("IV error");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
        (unsigned char*)key.data(), iv.data());

    std::vector<unsigned char> outbuf(plaintext.size()+EVP_MAX_BLOCK_LENGTH);
    int len1=0; 
    EVP_EncryptUpdate(ctx, outbuf.data(), &len1,
        (unsigned char*)plaintext.data(), plaintext.size());
    int len2=0;
    EVP_EncryptFinal_ex(ctx, outbuf.data()+len1, &len2);
    EVP_CIPHER_CTX_free(ctx);

    std::string result((char*)iv.data(), iv.size());
    result.append((char*)outbuf.data(), len1+len2);
    return result;
}

std::string decrypt(const std::string& ciphertext, const std::string& key) {
    if (key.size()!=32) throw std::runtime_error("Key must be 32 bytes");
    if (ciphertext.size()<16) throw std::runtime_error("Cipher too short");

    std::vector<unsigned char> iv(ciphertext.begin(), ciphertext.begin()+16);
    std::vector<unsigned char> data(ciphertext.begin()+16, ciphertext.end());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
        (unsigned char*)key.data(), iv.data());

    std::vector<unsigned char> outbuf(data.size()+EVP_MAX_BLOCK_LENGTH);
    int len1=0;
    EVP_DecryptUpdate(ctx, outbuf.data(), &len1, data.data(), data.size());
    int len2=0;
    EVP_DecryptFinal_ex(ctx, outbuf.data()+len1, &len2);
    EVP_CIPHER_CTX_free(ctx);

    return std::string((char*)outbuf.data(), len1+len2);
}

// helper: hex-таблица
static const char hexDigits[] = "0123456789ABCDEF";

std::string toHex(const std::string& data) {
    std::string out;
    out.reserve(data.size()*2);
    for (unsigned char c : data) {
        out.push_back(hexDigits[c>>4]);
        out.push_back(hexDigits[c&0x0F]);
    }
    return out;
}

std::string fromHex(const std::string& hex) {
    std::string out;
    out.reserve(hex.size()/2);
    auto hexVal = [](char c)->unsigned char {
        if (c>='0'&&c<='9') return c-'0';
        if (c>='A'&&c<='F') return c-'A'+10;
        if (c>='a'&&c<='f') return c-'a'+10;
        return 0;
    };
    for (size_t i = 0; i+1 < hex.size(); i += 2) {
        unsigned char hi = hexVal(hex[i]);
        unsigned char lo = hexVal(hex[i+1]);
        out.push_back((char)((hi<<4)|lo));
    }
    return out;
}