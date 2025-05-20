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

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "db.h"

#define PORT     12345
#define BACKLOG  10

// ——— Глобальные объекты —————————————————————————————————————————
static Database* db;
static std::atomic<bool> running{true};
static int serverSock = -1;

// SSL
static SSL_CTX* sslCtx = nullptr;
static std::mutex sslMtx;
static std::unordered_map<int,SSL*> sockToSsl;

// сокетные буферы и подписчики — без изменений
static std::mutex bufMtx;
static std::unordered_map<int,std::string> sockBuf;
static std::mutex subMtx;
static std::unordered_map<int,std::vector<int>> subscribers;
static std::mutex userMtx;
static std::unordered_map<int,int> socketToUser;
static std::unordered_map<int,std::vector<int>> userToSockets;

void init_openssl()
{
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    sslCtx = SSL_CTX_new(TLS_server_method());
    if (!sslCtx) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    // Задаём свой сертификат и приватный ключ:
    if (SSL_CTX_use_certificate_file(sslCtx, "server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(sslCtx,  "server.key",  SSL_FILETYPE_PEM) <= 0)
    {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    // Убеждаемся, что ключ подходит к сертификату:
    if (!SSL_CTX_check_private_key(sslCtx)) {
        fprintf(stderr,"Private key does not match the certificate public key\n");
        exit(1);
    }
}

static void sendSSL(int sock, const std::string& msg) {
    SSL* ssl = nullptr;
    {
        std::lock_guard lk(sslMtx);
        auto it = sockToSsl.find(sock);
        if (it!=sockToSsl.end()) ssl = it->second;
    }
    if (!ssl) return;
    SSL_write(ssl, msg.data(), msg.size());
    std::cerr << "[SERVER] sent to " << sock << ": " << msg;
}

// Удаляем клиента
void dropClient(int s) {
    {
        std::lock_guard lk(sslMtx);
        if (sockToSsl.count(s)) {
            SSL_shutdown(sockToSsl[s]);
            SSL_free(sockToSsl[s]);
            sockToSsl.erase(s);
        }
    }
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
    {
        std::lock_guard lk(userMtx);
        if (socketToUser.count(s)) {
            int u = socketToUser[s];
            socketToUser.erase(s);
            auto &v = userToSockets[u];
            v.erase(std::remove(v.begin(),v.end(),s),v.end());
        }
    }
    close(s);
}

// admin consoles
static void adminThread() {
    std::string line;
    while (running && std::getline(std::cin,line)) {
        if (line=="RESET") {
            bool ok = db->deleteEverything();
            if (!ok) std::cerr << "[SERVER] Error resetting\n";
            line="SHUTDOWN";
        }

        if (line=="SHUTDOWN") {
            running = false;
            shutdown(serverSock,SHUT_RDWR);
            close(serverSock);
            break;
        }
    }
}

static void clientHandler(int clientSock) {
    //борачиваем в SSL
    SSL* ssl = SSL_new(sslCtx);
    SSL_set_fd(ssl, clientSock);
    if (SSL_accept(ssl)<=0) {
        ERR_print_errors_fp(stderr);
        close(clientSock);
        return;
    }
    {
        std::lock_guard lk(sslMtx);
        sockToSsl[clientSock] = ssl;
    }

    char buf[2048];
    int userId = -1;

    while (true) {
        int r = SSL_read(ssl, buf, sizeof(buf));
        if (r<=0) break;
        {
            std::lock_guard lk(bufMtx);
            sockBuf[clientSock].append(buf, r);
        }
        // process lines
        while (true) {
             std::string line;
            {
                std::lock_guard lk(bufMtx);
                auto &b = sockBuf[clientSock];
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
                sendSSL(clientSock, ok ? "OK REG\n" : "ERROR USER_EXISTS\n");
            }
            else if (cmd=="LOGIN") {
                std::string u,p; iss>>u>>p;
                int id = db->authenticateUser(u,p);
                if (id>0) {
                    userId = id;
                    {
                        std::lock_guard ul(userMtx);
                        socketToUser[clientSock] = id;
                        userToSockets[id].push_back(clientSock);
                    }
                    sendSSL(clientSock,  "OK LOGIN\n");
                } else {
                    sendSSL(clientSock,  "ERROR NOT_CORRECT\n");
                }
            }
            else if (cmd=="LIST_CHATS") {
                if (userId<0) {
                    sendSSL(clientSock, "ERROR NOT_LOGGED\n");
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
                    for (int i = 0; i < members.size(); i++)
                    {
                        if (i != members.size() - 1) out << members[i] << ",";
                        else out << members[i];
                    }
                    out << ";";
                }
                // resubscribe
                {
                    std::lock_guard ukl(subMtx);
                    for (auto &kv:subscribers)
                        kv.second.erase(std::remove(kv.second.begin(),kv.second.end(), clientSock),
                                        kv.second.end());
                    for (auto &t:chats) {
                        int cid; bool isg; std::string name;
                        std::tie(cid,isg,name)=t;
                        subscribers[cid].push_back(clientSock);
                    }
                }

                std::string res = out.str();
                res.pop_back();
                res += "\n";

                sendSSL(clientSock,  res);
            }
            else if (cmd=="CREATE_CHAT") {
                if (userId<0) { sendSSL(clientSock, "ERROR NOT_LOGGED\n"); continue; }
                int isGroup; iss>>isGroup;
                if (!isGroup) {
                    int peer; iss>>peer;
                    // 1) проверка дубликата
                    int existing = db->findPrivateChat(userId, peer);
                    if (existing > 0) {
                        sendSSL(clientSock,  "ERROR CHAT_EXISTS\n");
                        continue;
                    }
                    // 2) создаём
                    int chatId = db->createChat(false, "");
                    db->addUserToChat(chatId, userId);
                    db->addUserToChat(chatId, peer);
                    sendSSL(clientSock, std::to_string(chatId) + "\n");
                    // 3) пуш
                    std::string push = "NEW_CHAT\n";
                    std::lock_guard ul(userMtx);
                    for (int u: {userId, peer})
                        for (auto s2: userToSockets[u])
                            sendSSL(s2, push);
                } else {
                    std::string rest; iss>>std::ws; std::getline(iss,rest);
                    std::istringstream is2(rest);
                    std::string gname; is2>>gname;
                    std::vector<int> members={userId};
                    int x;
                    while(is2>>x) members.push_back(x);
                    int cid = db->createChat(true,gname);
                    for(int u:members) db->addUserToChat(cid,u);
                    sendSSL(clientSock, std::to_string(cid)+"\n");
                    std::string push="NEW_CHAT\n";
                    std::lock_guard ul(userMtx);
                    for(int u:members)
                        for(int sock2:userToSockets[u])
                            sendSSL(sock2,push);
                }
            }
            else if (cmd=="SEND") {
                if (userId<0) { sendSSL(clientSock, "ERROR NOT_LOGGED\n"); continue; }
                int cid; iss>>cid;
                std::string msg; std::getline(iss,msg);
                if (!db->isUserInChat(cid,userId)) {
                    sendSSL(clientSock,"ERROR NO_CHAT_ACCESS\n");
                    continue;
                }
                int id = db->storeMessage(cid,userId,msg);
                
                std::ostringstream out;
                out << "OK SENT " << id << "\n";

                sendSSL(clientSock,id? out.str() :"ERROR\n");
                if (id) {
                    std::lock_guard sl(subMtx);
                    for (int sock2:subscribers[cid])
                            if (sock2 != clientSock)
                                sendSSL(sock2,"NEW_HISTORY " + std::to_string(cid) + "\n");
                }
            }
            else if (cmd=="HISTORY") {
                if (userId<0) { sendSSL(clientSock,"ERROR NOT_LOGGED\n"); continue; }
                int cid; iss>>cid;
                if (!db->isUserInChat(cid,userId)) {
                    sendSSL(clientSock,"ERROR NO_CHAT_ACCESS\n"); continue;
                }

                // Получаем два упорядоченных по времени списка:
                // 1) сообщения: tuple<msg_id, date, username, content>
                auto messages = db->getChatHistory(cid, userId);
                // 2) события: tuple<date, user_id, event_type>
                auto events   = db->getChatEvents(cid);

                // Объём будем складывать в одну очередь вариантов:
                struct Item {
                    std::string ts;
                    enum { MSG, EVT } type;
                    int         msg_id;       // только для MSG
                    std::string from;         // username или для EVT — имя пользователя
                    std::string text;         // для MSG — content; для EVT — "вошёл"/"покинул"
                };
                std::vector<Item> merged;
                merged.reserve(messages.size() + events.size());

                // Индексы для двух списков
                size_t i = 0, j = 0;

                // Функция, чтобы получить username по user_id
                auto userNameById = [&](int uid)->std::string {
                    return db->getUsername(uid);
                };

                // Собираем оба списка в общий, мёржим по ts
                while (i < messages.size() || j < events.size()) {
                    bool takeMsg = false;
                    if (i < messages.size() && j < events.size()) {
                        // сравниваем строки "YYYY-MM-DD HH:MM" лексиком
                        takeMsg = std::get<1>(messages[i]) <= std::get<0>(events[j]);
                    } else if (i < messages.size()) {
                        takeMsg = true;
                    }
                    if (takeMsg) {
                        Item it;
                        it.ts      = std::get<1>(messages[i]);
                        it.type    = Item::MSG;
                        it.msg_id  = std::get<0>(messages[i]);
                        it.from    = std::get<2>(messages[i]);  // username
                        it.text    = std::get<3>(messages[i]);  // content
                        merged.push_back(std::move(it));
                        ++i;
                    } else {
                        Item it;
                        it.ts      = std::get<0>(events[j]);
                        it.type    = Item::EVT;
                        it.from    = userNameById(std::get<1>(events[j]));
                        it.text    = (std::get<2>(events[j]) == "LEFT"
                                    ? "покинул(а) чат"
                                    : "вошёл в чат");
                        merged.push_back(std::move(it));
                        ++j;
                    }
                }

                // Формируем одну строку-ответ
                std::ostringstream out;
                out << "HISTORY ";
                for (auto &it : merged) {
                    if (it.type == Item::MSG) {
                        out << "[" << it.ts << "] "
                            << it.from << ": "
                            << it.text
                            << " (id=" << it.msg_id << ");";
                    } else {
                        out << "[" << it.ts << "] * "
                            << it.from << " "
                            << it.text << ";";
                    }
                }
                out << "\n";
                sendSSL(clientSock, out.str());
            }
            // DELETE — удаление у себя
            else if (cmd == "DELETE") {
                int msg_id; 
                iss >> msg_id;
                int sender = db->getMessageSender(msg_id);
                if (sender == userId) {
                    bool ok = db->deleteMessageForUser(msg_id, userId);
                    std::ostringstream notif;
                    notif << "MSG_DELETED " << msg_id << "\n";
                    sendSSL(clientSock, ok ? notif.str() : "ERROR\n");
                } else {
                    sendSSL(clientSock, "ERROR NO_RIGHTS\n");
                }
            }
            else if (cmd == "DELETE_GLOBAL") {
                int msg_id;
                iss >> msg_id;
                int sender = db->getMessageSender(msg_id);
                if (sender != userId) {
                    sendSSL(clientSock, "ERROR NO_RIGHTS\n");
                    continue;
                }
                // 1) пометить в БД
                bool ok = db->deleteMessageGlobal(msg_id);
                if (!ok) {
                    sendSSL(clientSock, "ERROR\n");
                    continue;
                }

                // 2) разослать всем подписчикам чата уведомление
                //    сначала найдём chat_id (можно хранить map msg->chat, 
                //    но проще — запросить в БД)
                int chat_id = db->getChatIdByMessage(msg_id);
                std::ostringstream notif;
                notif << "MSG_DELETED " << msg_id << "\n";

                std::lock_guard lk(subMtx);
                auto it = subscribers.find(chat_id);
                if (it != subscribers.end()) {
                    for (int sock2 : it->second) {
                        sendSSL(sock2, notif.str());
                    }
                }
            }
            else if (cmd=="LEAVE_CHAT") {
                int cid; iss>>cid;
                if (userId<0 || !db->isUserInChat(cid,userId)) {
                    sendSSL(clientSock, "ERROR\n");
                } else {
                    bool ok = db->removeUserFromChat(cid, userId);
                    sendSSL(clientSock, ok ? "OK LEFT\n" : "ERROR\n");
                    if (ok) {
                        // узнаём имя
                        std::string name = db->getUsername(userId);
                        // формируем уведомление
                        std::ostringstream nt;
                        auto now = std::chrono::system_clock::now();
                        std::time_t t  = std::chrono::system_clock::to_time_t(now);
                        std::tm tm; localtime_r(&t, &tm);
                        char buf[30];
                        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
                        std::string ts(buf);
                        nt<<"USER_LEFT "<<cid<<" "<<name<<" "<< ts << "\n";
                        std::string notif = nt.str();
                        // шаём всем оставшимся подписчикам
                        std::lock_guard lk(subMtx);
                        for(int sock2: subscribers[cid])
                            if (sock2 != clientSock)
                                sendSSL(sock2, notif);
                        // и удаляем из подписчиков этого клиента
                        auto &vec = subscribers[cid];
                        vec.erase(std::remove(vec.begin(), vec.end(), clientSock), vec.end());
                    }
                }
            }
            else if (cmd=="GET_USER_ID") {
                std::string nm; iss>>nm;
                int uid = db->getUserIdByName(nm);
                sendSSL(clientSock,uid>0 ? std::to_string(uid)+"\n" : "ERROR NO_SUCH_USER\n");
            }
            else {
                sendSSL(clientSock, "ERROR UNKNOWN\n");
            }
        }
    }

    dropClient(clientSock);
    {
        std::lock_guard ul(userMtx);
        auto it = socketToUser.find(clientSock);
        if (it!=socketToUser.end()) {
            int u = it->second;
            socketToUser.erase(it);
            auto &v = userToSockets[u];
            v.erase(std::remove(v.begin(),v.end(),clientSock),v.end());
        }
    }
}

int main(){
    // 1) Init OpenSSL
    init_openssl();

    // 2) Init DB
    db = new Database("host=localhost dbname=chatdb user=chatuser password=123");

    // 3) Set up TCP listener
    serverSock = socket(AF_INET,SOCK_STREAM,0);
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(PORT);
    addr.sin_addr.s_addr=INADDR_ANY;
    bind(serverSock,(sockaddr*)&addr,sizeof(addr));
    listen(serverSock,BACKLOG);
    std::cout<<"Listening on "<<PORT<<"\n";

    // 4) Админ‑поток
    std::thread(adminThread).detach();

    // 5) Основной цикл
    while (running) {
        int clientSock = accept(serverSock,nullptr,nullptr);
        if (clientSock<0) break;

        std::thread(clientHandler, clientSock).detach();
    }

    // Cleanup
    SSL_CTX_free(sslCtx);
    EVP_cleanup();
    delete db;
    return 0;
}
