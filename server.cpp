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
#include <algorithm>
#include <tuple>
#include <sstream>
#include <ctime>

#include "db.h"
#include "crypto.h"

#define PORT     12345
#define BACKLOG  10

static Database* db;
static const std::string AES_KEY = "01234567890123456789012345678901";
static std::atomic<bool> running{true};
static int serverSock = -1;

// --- Socket buffers
static std::mutex bufMtx;
static std::unordered_map<int,std::string> sockBuf;

// --- Subscribers for pushes: chat_id -> sockets
static std::mutex subMtx;
static std::unordered_map<int,std::vector<int>> subscribers;

// --- User ↔ sockets
static std::mutex userMtx;
static std::unordered_map<int,int> socketToUser;
static std::unordered_map<int,std::vector<int>> userToSockets;

// reliable send
static void sendStr(int s, const std::string &m) {
    size_t sent = 0;
    while (sent < m.size()) {
        ssize_t w = send(s, m.data()+sent, m.size()-sent, 0);
        if (w<=0) break;
        sent += w;
    }
    std::cerr << "[SERVER] sent: " << m;
}

// remove client from buffers & subscribers
static void dropClient(int s) {
    {
        std::lock_guard lk(bufMtx);
        sockBuf.erase(s);
    }
    {
        std::lock_guard lk(subMtx);
        for (auto &kv : subscribers)
            kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), s),
                            kv.second.end());
    }
}

// admin consoles
static void adminThread() {
    std::string line;
    while (running && std::getline(std::cin,line)) {
        if (line=="RESET") {
            db->deleteEverything();
            line = "SHUTDOWN";
        }

        if (line=="SHUTDOWN") {
            running = false;
            shutdown(serverSock,SHUT_RDWR);
            close(serverSock);
            break;
        }
    }
}

static void clientHandler(int s) {
    char buf[2048];
    int userId = -1;

    while (true) {
        ssize_t r = recv(s, buf, sizeof(buf), 0);
        if (r<=0) break;
        {
            std::lock_guard lk(bufMtx);
            sockBuf[s].append(buf, r);
        }
        // process lines
        while (true) {
            std::string line;
            {
                std::lock_guard lk(bufMtx);
                auto &b = sockBuf[s];
                auto p = b.find('\n');
                if (p==std::string::npos) break;
                line = b.substr(0,p);
                b.erase(0,p+1);
            }
            if (line.size() && line.back()=='\r') line.pop_back();
            std::istringstream iss(line);
            std::string cmd; iss>>cmd;

            if (cmd=="REGISTER") {
                std::string u,p; iss>>u>>p;
                bool ok = db->registerUser(u,p);
                sendStr(s, ok ? "OK REG\n" : "ERROR USER_EXISTS\n");
            }
            else if (cmd=="LOGIN") {
                std::string u,p; iss>>u>>p;
                int id = db->authenticateUser(u,p);
                if (id>0) {
                    userId = id;
                    {
                        std::lock_guard ul(userMtx);
                        socketToUser[s] = id;
                        userToSockets[id].push_back(s);
                    }
                    sendStr(s, "OK LOGIN\n");
                } else {
                    sendStr(s, "ERROR\n");
                }
            }
            else if (cmd=="LIST_CHATS") {
                if (userId<0) {
                    sendStr(s,"ERROR Not logged\n");
                    continue;
                }
                auto chats = db->listUserChats(userId);
                std::ostringstream out;
                out<<"CHATS ";
                for (auto &t:chats) {
                    int cid; bool isg; std::string name;
                    std::tie(cid,isg,name) = t;
                    out<<cid<<":"<<(isg?"1":"0")<<":"<<name<<":";
                    auto members = db->chatMembers(cid);
                    for (auto &member : members)
                        out << member << ",";
                    out << ";";
                }
                // resubscribe
                {
                    std::lock_guard ukl(subMtx);
                    for (auto &kv:subscribers)
                        kv.second.erase(std::remove(kv.second.begin(),kv.second.end(),s),
                                        kv.second.end());
                    for (auto &t:chats) {
                        int cid; bool isg; std::string name;
                        std::tie(cid,isg,name)=t;
                        subscribers[cid].push_back(s);
                    }
                }

                std::string res = out.str();
                res.pop_back();
                res += "\n";

                sendStr(s, res);
            }
            else if (cmd=="CREATE_CHAT") {
                if (userId<0) { sendStr(s,"ERROR Not logged\n"); continue; }
                int isGroup; iss>>isGroup;
                if (!isGroup) {
                    int peer; iss>>peer;
                    // 1) проверка дубликата
                    int existing = db->findPrivateChat(userId, peer);
                    if (existing > 0) {
                        sendStr(s, "ERROR CHAT_EXISTS\n");
                        continue;
                    }
                    // 2) создаём
                    int chatId = db->createChat(false, "");
                    db->addUserToChat(chatId, userId);
                    db->addUserToChat(chatId, peer);
                    sendStr(s, std::to_string(chatId) + "\n");
                    // 3) пуш
                    std::string push = "NEW_CHAT\n";
                    std::lock_guard ul(userMtx);
                    for (int u: {userId, peer})
                        for (auto s2: userToSockets[u])
                            sendStr(s2, push);
                } else {
                    // group: first token is group name, then ids
                    std::string rest; iss>>std::ws; std::getline(iss,rest);
                    std::istringstream is2(rest);
                    std::string gname; is2>>gname;
                    std::vector<int> members={userId};
                    int x;
                    while(is2>>x) members.push_back(x);
                    int cid = db->createChat(true,gname);
                    for(int u:members) db->addUserToChat(cid,u);
                    sendStr(s, std::to_string(cid)+"\n");
                    std::string push="NEW_CHAT\n";
                    std::lock_guard ul(userMtx);
                    for(int u:members)
                        for(int sock2:userToSockets[u])
                            sendStr(sock2,push);
                }
            }
            else if (cmd=="SEND") {
                if (userId<0) { sendStr(s,"ERROR Not logged\n"); continue; }
                int cid; iss>>cid;
                std::string hex; std::getline(iss,hex);
                if (!hex.empty()&&hex.front()==' ') hex.erase(0,1);
                std::string msg = decrypt(fromHex(hex),AES_KEY);
                if (!db->isUserInChat(cid,userId)) {
                    sendStr(s,"ERROR No chat access\n");
                    continue;
                }
                bool ok = db->storeMessage(cid,userId,msg);
                sendStr(s, ok?"OK SENT\n":"ERROR\n");
                if (ok) {
                    std::lock_guard sl(subMtx);
                    for (int sock2:subscribers[cid])
                            if (sock2 != s)
                                sendStr(sock2,"NEW_HISTORY " + std::to_string(cid) + "\n");
                }
            }
            else if (cmd=="HISTORY") {
                if (userId<0) { sendStr(s,"ERROR Not logged\n"); continue; }
                int cid; iss>>cid;
                if (!db->isUserInChat(cid,userId)) {
                    sendStr(s,"ERROR No chat access\n"); continue;
                }

                std::ostringstream out;

                out << "HISTORY "; 

                auto messages = db->getChatHistory(cid,userId);
                for (auto &message : messages) {
                    std::string date; std::string name;; std::string content;
                    std::tie(date,name,content) = message;
                    out << "[" << date << "]" << " " << name << ": " << content << ";";
                }

                std::string res = out.str();
                res.pop_back(); //last ;

                sendStr(s, res);
            }
            // DELETE — удаление у себя
            else if (cmd == "DELETE") {
                int msg_id; 
                iss >> msg_id;
                int sender = db->getMessageSender(msg_id);
                if (sender == userId) {
                    bool ok = db->deleteMessageForUser(msg_id, userId);
                    sendStr(s, ok ? "OK\n" : "ERROR\n");
                } else {
                    sendStr(s, "ERROR No rights\n");
                }
            }
            // DELETE_GLOBAL — удаление для всех
            else if (cmd == "DELETE_GLOBAL") {
                int msg_id; 
                iss >> msg_id;
                int sender = db->getMessageSender(msg_id);
                if (sender == userId) {
                    bool ok = db->deleteMessageGlobal(msg_id);
                    sendStr(s, ok ? "OK\n" : "ERROR\n");
                } else {
                    sendStr(s, "ERROR No rights\n");
                }
            }
            else if (cmd=="GET_USER_ID") {
                std::string nm; iss>>nm;
                int uid = db->getUserIdByName(nm);
                sendStr(s, uid>0 ? std::to_string(uid)+"\n" : "ERROR NO_SUCH_USER\n");
            }
            else {
                sendStr(s,"ERROR Unknown\n");
            }
        }
    }

    dropClient(s);
    close(s);
    {
        std::lock_guard ul(userMtx);
        auto it = socketToUser.find(s);
        if (it!=socketToUser.end()) {
            int u = it->second;
            socketToUser.erase(it);
            auto &v = userToSockets[u];
            v.erase(std::remove(v.begin(),v.end(),s),v.end());
        }
    }
}

int main(){
    std::thread(adminThread).detach();
    db = new Database("host=localhost dbname=chatdb user=chatuser password=123");

    serverSock = socket(AF_INET,SOCK_STREAM,0);
    if (serverSock<0){ perror("socket"); return 1; }
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(PORT);
    addr.sin_addr.s_addr=INADDR_ANY;
    if (bind(serverSock,(sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 2; }
    if (listen(serverSock,BACKLOG)<0){ perror("listen"); return 3; }
    std::cout<<"Listening on "<<PORT<<"\n";
    while (running) {
        int cl = accept(serverSock,nullptr,nullptr);
        if (!running) break;
        if (cl<0) { if (running) perror("accept"); break; }
        std::thread(clientHandler,cl).detach();
    }
    delete db;
    return 0;
}
