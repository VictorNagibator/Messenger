// client.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>

#include "crypto.h"

#define PORT 12345
#define BUF_SIZE 8192

static const std::string aesKey = "01234567890123456789012345678901";

int main() {
    // 1) Создаём сокет и подключаемся
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 2;
    }

    std::cout << "Commands:\n"
              << " REGISTER <user> <pass>\n"
              << " LOGIN    <user> <pass>\n"
              << " LIST_USERS\n"
              << " CREATE_CHAT <0|1> [name if group] <member_ids...>\n"
              << " SEND     <chat_id> <message>\n"
              << " HISTORY  <chat_id>\n"
              << " DELETE   <msg_id>\n"
              << " EXIT\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == "EXIT") break;

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;

        std::string out;
        if (cmd == "SEND") {
            // SEND <chat_id> <message>
            int cid; iss >> cid;
            std::string msg;
            std::getline(iss, msg);
            // шифруем и отправляем
            std::string enc = encrypt(msg, aesKey);
            out = "SEND " + std::to_string(cid) + enc + "\n";

        } else {
            // для всех остальных команд — просто добавляем \n
            out = line + "\n";
        }

        // отправляем команду серверу
        if (send(sock, out.c_str(), out.size(), 0) != (ssize_t)out.size()) {
            perror("send");
            break;
        }

        // читаем ответ (до \n или весь блок)
        char buf[BUF_SIZE];
        ssize_t r = recv(sock, buf, sizeof(buf)-1, 0);
        if (r <= 0) {
            std::cout << "Connection closed by server\n";
            break;
        }
        buf[r] = '\0';
        std::cout << buf;
    }

    close(sock);
    return 0;
}
