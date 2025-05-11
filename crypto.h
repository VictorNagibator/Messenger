#ifndef CRYPTO_H
#define CRYPTO_H

#include <string>

// Ключ должен быть ровно 32 байта
std::string encrypt(const std::string& plaintext, const std::string& key);
std::string decrypt(const std::string& ciphertext, const std::string& key);

#endif // CRYPTO_H