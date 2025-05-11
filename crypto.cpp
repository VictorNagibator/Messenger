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
