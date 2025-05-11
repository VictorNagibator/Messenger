// server.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <sstream>
#include <iostream>
#include <atomic>
#include <cstring>

#include "db.h"
#include "crypto.h"

#define PORT 12345
#define BUF_SIZE 8192

static Database* db;
static const std::string aesKey = "01234567890123456789012345678901";

// Флаг работы сервера
static std::atomic<bool> running{true};

// Закрываем сокет прослушивания при shutdown
static int serverSock = -1;

void adminConsole() {
    std::string line;
    while (running.load()) {
        std::getline(std::cin, line);
        if (line == "SHUTDOWN") {
            std::cout << "[ADMIN] SHUTDOWN received, stopping server...\n";
            running.store(false);
            if (serverSock != -1) {
                // Сначала говорим, что больше не принимаем соединения
                shutdown(serverSock, SHUT_RDWR);
                // А затем закрываем дескриптор
                close(serverSock);
            }
            break;
        }
    }
}

void clientThread(int sock) {
    char buf[BUF_SIZE];
    int userId = -1;
    while (true) {
        ssize_t r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) break;
        std::string cmd(buf, r);
        std::istringstream iss(cmd);
        std::string token; iss >> token;

        if (token == "REGISTER") {
            std::string u,p; iss >> u >> p;
            bool ok = db->registerUser(u,p);
            if (ok) {
                int id = db->authenticateUser(u,p);
                std::ostringstream resp;
                resp << "OK " << id << "\n";
                send(sock, resp.str().c_str(), resp.str().size(), 0);
            } else {
                send(sock, "ERROR USER_EXISTS\n", 18, 0);
            }
        }
        else if (token=="LOGIN") {
            std::string u,p; iss>>u>>p;
            int id = db->authenticateUser(u,p);
            if (id > 0) {
                userId = id;
                std::ostringstream resp;
                resp << "OK " << id << "\n";
                send(sock, resp.str().c_str(), resp.str().size(), 0);
            } else {
                send(sock, "ERROR\n", 6, 0);
            }
        }
        else if (token=="LIST_USERS") {
            auto users = db->listUsers();
            std::ostringstream out;
            for (auto& pr : users) {
                out << pr.first << " " << pr.second << "\n";
            }
            send(sock, out.str().c_str(), out.str().size(), 0);
        }
        else if (token == "CREATE_CHAT") {
            if (userId < 0) {
                send(sock, "ERROR Not logged\n", 17, 0);
                continue;
            }
            int isGroup; iss >> isGroup;
            std::string chatName;
            if (isGroup) {
                // читаем одно слово как имя
                iss >> std::ws;
                std::getline(iss, chatName, ' ');
            }
            // создаём чат
            int chat_id = db->createChat(isGroup, chatName);

            // всегда добавляем создателя
            db->addUserToChat(chat_id, userId);

            // читаем всех user_id из оставшейся строки
            int uid;
            while (iss >> uid) {
                db->addUserToChat(chat_id, uid);
            }

            std::ostringstream resp; resp << chat_id << "\n";
            send(sock, resp.str().c_str(), resp.str().size(), 0);
        }
        else if (token=="SEND") {
            if (userId<0) { send(sock,"ERROR Not logged\n",18,0); continue; }
            int chat_id; iss>>chat_id;
            std::string enc; iss>>std::ws; std::getline(iss,enc);
            if (!db->isUserInChat(chat_id, userId)) {
                send(sock,"ERROR No chat access\n",21,0); continue;
            }
            std::string text = decrypt(enc, aesKey);
            bool ok = db->storeMessage(chat_id, userId, text);
            send(sock, ok?"OK\n":"ERROR\n", ok?3:6, 0);
        }
        else if (token=="HISTORY") {
            if (userId<0) { send(sock,"ERROR Not logged\n",18,0); continue; }
            int chat_id; iss>>chat_id;
            if (!db->isUserInChat(chat_id, userId)) {
                send(sock,"ERROR No chat access\n",21,0); continue;
            }
            std::string hist = db->getChatHistory(chat_id);
            send(sock, hist.c_str(), hist.size(), 0);
        }
        else if (token=="DELETE") {
            if (userId<0) { send(sock,"ERROR Not logged\n",18,0); continue; }
            int msg_id; iss>>msg_id;
            bool ok = db->deleteMessage(msg_id);
            send(sock, ok?"OK\n":"ERROR\n", ok?3:6, 0);
        }
        else {
            send(sock,"ERROR Unknown cmd\n",18,0);
        }
    }
    close(sock);
}

int main(){
    // Запуск потока админ-консоли
    std::thread(adminConsole).detach();

    const std::string conninfo =
      "host=localhost dbname=chatdb user=chatuser password=123";
    db = new Database(conninfo);

    serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) { perror("socket"); return 1; }

    sockaddr_in addr{}; 
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr))<0) {
        perror("bind"); return 2;
    }
    if (listen(serverSock, 10) < 0) {
        perror("listen"); return 3;
    }

    std::cout << "Server listening on port " << PORT 
              << "  (type SHUTDOWN + Enter to stop)\n";

    // Приём клиентских соединений
    while (running.load()) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (!running.load()) break;  // если shutdown
        if (clientSock < 0) {
            if (running.load()) perror("accept");
            break;
        }
        std::thread(clientThread, clientSock).detach();
    }

    std::cout << "Server is shutting down...\n";
    delete db;
    return 0;
}