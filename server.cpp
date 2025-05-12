// server.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>
#include <string>
#include <iostream>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>    // std::remove
#include <tuple>        // std::tie
#include <sstream>
#include <ctime>        // std::time, std::strftime

#include "db.h"
#include "crypto.h"

#define PORT     12345
#define BACKLOG  10

static Database* db;
static const std::string aesKey = "01234567890123456789012345678901";
static std::atomic<bool> running{true};
static int serverSock = -1;

// client socket → partial buffer
static std::mutex bufMtx;
static std::unordered_map<int, std::string> sockBuffers;

// chat_id → subscribers
static std::mutex subMtx;
static std::unordered_map<int, std::vector<int>> subscribers;

// socket → userId
static std::mutex userMtx;
static std::unordered_map<int,int> socketToUser;      // sock -> userId
static std::unordered_map<int,std::vector<int>> userToSockets; // userId -> [sock]

// Получить текущее время в формате YYYY-MM-DD HH24:MI
static std::string currentTimestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

void sendStr(int sock, const std::string &s) {
    size_t total = 0;
    while (total < s.size()) {
        ssize_t sent = send(sock, s.data()+total, s.size()-total, 0);
        if (sent <= 0) break;
        total += sent;
    }
}

void removeClient(int sock) {
    std::lock_guard<std::mutex> lk(bufMtx);
    sockBuffers.erase(sock);
    // и отписываем от подписок
    std::lock_guard<std::mutex> lk2(subMtx);
    for (auto &kv : subscribers) {
        auto &vec = kv.second;
        vec.erase(std::remove(vec.begin(), vec.end(), sock), vec.end());
    }
}

void adminConsole() {
    std::string line;
    while (running.load()) {
        if (!std::getline(std::cin, line)) break;
        if (line == "SHUTDOWN") {
            std::cout << "[ADMIN] SHUTDOWN received, stopping server...\n";
            running.store(false);
            if (serverSock != -1) {
                shutdown(serverSock, SHUT_RDWR);
                close(serverSock);
            }
            break;
        }
    }
}

void clientThread(int sock) {
    char tmp[1024];
    int userId = -1;

    while (true) {
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;

        {
            std::lock_guard<std::mutex> lk(bufMtx);
            sockBuffers[sock].append(tmp, n);
        }

        while (true) {
            std::string line;
            {
                std::lock_guard<std::mutex> lk(bufMtx);
                auto &buf = sockBuffers[sock];
                auto pos = buf.find('\n');
                if (pos == std::string::npos) break;
                line = buf.substr(0, pos);
                buf.erase(0, pos+1);
            }
            if (!line.empty() && line.back()=='\r') line.pop_back();

            std::cerr << "[SERVER] Got: \"" << line << "\"\n";
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            if (cmd == "REGISTER") {
                std::string u,p; iss>>u>>p;
                bool ok = db->registerUser(u,p);
                if (ok) {
                    int id = db->authenticateUser(u,p);
                    sendStr(sock, "OK " + std::to_string(id) + "\n");
                } else {
                    sendStr(sock, "ERROR USER_EXISTS\n");
                }
            }
            else if (cmd == "LOGIN") {
                std::string u,p; iss>>u>>p;
                int id = db->authenticateUser(u,p);
                if (id>0) {
                    userId = id;
                    {
                        std::lock_guard lk(userMtx);
                        socketToUser[sock] = userId;
                        userToSockets[userId].push_back(sock);
                    }
                    sendStr(sock, "OK " + std::to_string(id) + "\n");
                } else {
                    sendStr(sock, "ERROR\n");
                }   
            }
            else if (cmd == "LIST_CHATS") {
                if (userId<0) {
                    sendStr(sock, "ERROR Not logged\n");
                } else {
                    auto chats = db->listUserChats(userId);
                    std::ostringstream out;
                    out << "CHATS ";
                    for (auto &t : chats) {
                        int cid; bool isg; std::string name;
                        std::tie(cid,isg,name) = t;
                        out << cid << ":" << (isg?"1":"0") << ":" << name << ";";
                    }
                    out << "\n";

                    // подпишем на пуши
                    {
                        std::lock_guard<std::mutex> lk(subMtx);
                        // очистка
                        for (auto &kv : subscribers) {
                            auto &v = kv.second;
                            v.erase(std::remove(v.begin(), v.end(), sock), v.end());
                        }
                        // новая подписка
                        for (auto &t : chats) {
                            int cid; bool isg; std::string name;
                            std::tie(cid,isg,name) = t;
                            subscribers[cid].push_back(sock);
                        }
                    }
                    sendStr(sock, out.str());
                }
            }
            else if (cmd == "SEND") {
                if (userId<0) { sendStr(sock,"ERROR Not logged\n"); continue; }
                int chat_id; iss>>chat_id;
                std::string hex; std::getline(iss,hex);
                if (!hex.empty()&&hex.front()==' ') hex.erase(0,1);
                std::string msg = decrypt(fromHex(hex), aesKey);

                if (!db->isUserInChat(chat_id,userId)) {
                    sendStr(sock,"ERROR No chat access\n");
                    continue;
                }
                bool ok = db->storeMessage(chat_id, userId, msg);
                sendStr(sock, ok?"OK\n":"ERROR\n");

                if (ok) {
                    std::string ts = currentTimestamp();
                    std::ostringstream push;
                    push << "[" << ts << "] "
                         << db->getUsername(userId)
                         << ": " << msg << "\n";
                    auto s = push.str();

                    std::lock_guard<std::mutex> lk(subMtx);
                    for (int peer : subscribers[chat_id]) {
                        if (peer!=sock) sendStr(peer, s);
                    }
                }
            }
            else if (cmd == "HISTORY") {
                if (userId<0) { sendStr(sock,"ERROR Not logged\n"); continue; }
                int chat_id; iss>>chat_id;
                if (!db->isUserInChat(chat_id,userId)) {
                    sendStr(sock,"ERROR No chat access\n");
                    continue;
                }
                sendStr(sock, db->getChatHistory(chat_id));
            }
            else if (cmd == "CREATE_CHAT") {
                if (userId<0) { sendStr(sock,"ERROR Not logged\n"); continue; }
                int isG; iss >> isG;

                // 1) читаем ОДНО слово как название группы
                std::string chatName;
                if (isG) {
                    iss >> chatName;  // вместо getline, чтобы НЕ схватить ID
                }

                // 2) создаём чат
                int chat_id = db->createChat(isG, chatName);

                // 3) всегда добавляем создателя
                db->addUserToChat(chat_id, userId);
                std::vector<int> members = { userId };

                // 4) добавляем остальных участников из оставшихся токенов
                int uid;
                while (iss >> uid) {
                    db->addUserToChat(chat_id, uid);
                    members.push_back(uid);
                }

                // 5) возвращаем клиенту только ID нового чата
                sendStr(sock, std::to_string(chat_id) + "\n");

                // 6) PUSH новым участникам: всем соединениям этих userId
                std::string chunk = std::to_string(chat_id) + ":" +
                                    (isG?"1":"0") + ":" + chatName + ";";
                std::string push  = "NEW_CHAT " + chunk + "\n";

                std::lock_guard<std::mutex> lk(userMtx);
                for (int peerId : members) {
                    for (int peerSock : userToSockets[peerId]) {
                        sendStr(peerSock, push);
                    }
                }
            }
            else if (cmd == "DELETE") {
                int mid; iss>>mid;
                sendStr(sock, db->deleteMessage(mid) ? "OK\n":"ERROR\n");
            }
            else if (cmd == "GET_USER_ID") {
                std::string name; iss>>name;
                int uid = db->getUserIdByName(name);
                sendStr(sock, uid>0 ? std::to_string(uid)+"\n" : "ERROR NO_SUCH_USER\n");
            }
            else {
                sendStr(sock,"ERROR Unknown cmd\n");
            }
        }
    }

    std::cerr<<"[SERVER] Disconnect sock "<<sock<<"\n";
    removeClient(sock);
    close(sock);
    {
        std::lock_guard lk(userMtx);
        auto it = socketToUser.find(sock);
        if (it != socketToUser.end()) {
            int uid = it->second;
            socketToUser.erase(it);
            auto &v = userToSockets[uid];
            v.erase(std::remove(v.begin(), v.end(), sock), v.end());
    }
}
}

int main(){
    std::thread(adminConsole).detach();

    db = new Database("host=localhost dbname=chatdb user=chatuser password=123");

    serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock<0) { perror("socket"); return 1; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock,(sockaddr*)&addr,sizeof(addr))<0){ perror("bind");return 2; }
    if (listen(serverSock,BACKLOG)<0){ perror("listen");return 3; }

    std::cout<<"Server listening on port "<<PORT<<"\n";
    while (running.load()) {
        int client = accept(serverSock,nullptr,nullptr);
        if (!running.load()) break;
        if (client<0) { if (running.load()) perror("accept"); break; }
        std::cerr<<"[SERVER] New sock "<<client<<"\n";
        std::thread(clientThread, client).detach();
    }

    std::cout<<"Server shutting down\n";
    delete db;
    return 0;
}