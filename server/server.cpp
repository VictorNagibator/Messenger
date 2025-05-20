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

#define PORT 12345
#define BACKLOG 10

//Основной объект работы с БД
static Database* db;

//Флаг работы сервера и сокет слушателя
static std::atomic<bool> running{true};
static int serverSock = -1;

//Контекст и словарь SSL-сокетов
static SSL_CTX* sslCtx = nullptr;
static std::mutex sslMtx;
static std::unordered_map<int, SSL*> sockToSsl;

//Буферизация и подписчики для каждого клиента
static std::mutex bufMtx;
static std::unordered_map<int, std::string> sockBuf;
static std::mutex subMtx;
static std::unordered_map<int, std::vector<int>> subscribers;

//Отображение клиент <-> пользователь
static std::mutex userMtx;
static std::unordered_map<int, int> socketToUser;
static std::unordered_map<int, std::vector<int>> userToSockets;

//Инициализация OpenSSL: создаём контекст, загружаем сертификат/ключ
void init_openssl()
{
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    //Создаем контекст
    sslCtx = SSL_CTX_new(TLS_server_method());
    if (!sslCtx) {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    //Загружаем серверный сертификат и ключ (заранее их уже создали)
    if (SSL_CTX_use_certificate_file(sslCtx, "server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(sslCtx,  "server.key",  SSL_FILETYPE_PEM) <= 0)
    {
        ERR_print_errors_fp(stderr);
        exit(1);
    }

    //Проверяем соответствие сертификата и ключа
    if (!SSL_CTX_check_private_key(sslCtx)) {
        std::cerr <<
                "Private key does not match the certificate public key\n";
        exit(1);
    }
}


//Отправка строки по SSL — берём SSL* из sockToSsl по номеру сокета
static void sendSSL(int sock, const std::string& msg) {
    //Каждому клиентскому сокету мы при успешном рукопожатии сохраняем
    //указатель SSL* в map<int,SSL*> sockToSsl
    SSL* ssl = nullptr;
    {
        //Захватываем мьютекс, чтобы безопасно читать из общей структуры sockToSsl
        std::lock_guard lk(sslMtx);
        auto it = sockToSsl.find(sock);
        if (it!=sockToSsl.end()) ssl = it->second;
    }

    //Если SSL* не нашли, выходим, ничего не отправляя
    if (!ssl) return;

    //Пишем в SSL: автоматически шифруется и отправляется по TCP
    SSL_write(ssl, msg.data(), msg.size());
}

//Корректно выкидываем клиента: SSL_shutdown, чистим буферы, подписки и закрываем TCP
void dropClient(int s) {
    //1) TLS: завершение и освобождение структуры SSL*
    {
        std::lock_guard lk(sslMtx);
        if (sockToSsl.count(s)) {
            SSL_shutdown(sockToSsl[s]);
            SSL_free(sockToSsl[s]);
            sockToSsl.erase(s);
        }
    }
    //2) Буфер входящих данных
    {
        std::lock_guard lk(bufMtx);
        sockBuf.erase(s);
    }
    //3) Отписываем из подписок на чаты
    {
        std::lock_guard lk(subMtx);
        for (auto &kv : subscribers)
            kv.second.erase(std::remove(kv.second.begin(), kv.second.end(), s),
                            kv.second.end());
    }
    //4) Убираем связь socket->user и user->socket
    {
        std::lock_guard lk(userMtx);
        if (socketToUser.count(s)) {
            int u = socketToUser[s];
            socketToUser.erase(s);
            auto &v = userToSockets[u];
            v.erase(std::remove(v.begin(),v.end(),s),v.end());
        }
    }
    //5) Закрываем TCP‑сокет
    close(s);
}

//Админ‑поток, читает из stdin строки RESET/SHUTDOWN
//RESET — чистит всё в БД и затем SHUTDOWN
//SHUTDOWN — завершает основной accept‑цикл
static void adminThread() {
    std::string line;
    while (running && std::getline(std::cin,line)) {
        if (line == "RESET") {
            //Полная очистка БД
            bool ok = db->deleteEverything();
            if (!ok) std::cerr << "[SERVER] Error resetting\n";
            line = "SHUTDOWN";
        }

        if (line == "SHUTDOWN") {
            //Завершаем основной цикл: прерываем accept() и закрываем listen‑сокет
            running = false;
            shutdown(serverSock,SHUT_RDWR);
            close(serverSock);
            break;
        }
    }
}

//Обработчик одного клиентского соединения (каждому клиенту свой поток)
static void clientHandler(int clientSock) {
    //1) Обвёртка TCP в TLS
    SSL* ssl = SSL_new(sslCtx);
    SSL_set_fd(ssl, clientSock);
    if (SSL_accept(ssl) <= 0) {
        //Не удалось пройти TLS рукопожатие
        ERR_print_errors_fp(stderr);
        close(clientSock);
        return;
    }
    //Сохраняем SSL* для этого сокета
    {
        std::lock_guard lk(sslMtx);
        sockToSsl[clientSock] = ssl;
    }

    //Буфер приёма данных и идентификатор залогиненного пользователя
    char buf[2048];
    int userId = -1;

    //Основной цикл чтения от клиента
    while (true) {
        int r = SSL_read(ssl, buf, sizeof(buf));
        if (r <= 0) break; //клиент отключился или ошибка

        //Добавляем прочитанные байты в строковый буфер
        {
            std::lock_guard lk(bufMtx);
            sockBuf[clientSock].append(buf, r);
            //При чтении из SSL‑сокета (SSL_read) можно получить любую часть отправленного сообщения 
            //возможно целую строку, а возможно только её кусок
            //TCP (и TLS над ним) гарантирует, что байты придут в том же порядке, но не что придут одним куском
        }

        //Разбираем буфер по строкам '\n'
        while (true) {
            std::string line;
            {
                //Берем только часть до \n
                std::lock_guard lk(bufMtx);
                auto &b = sockBuf[clientSock];
                auto p = b.find('\n');
                if (p == std::string::npos) break; //Выходим, если нет \n 
                line = b.substr(0, p);
                b.erase(0,p+1);
            }

            //Убираем возможный '\r'
            if (line.size() && line.back() == '\r') line.pop_back();

            //Парсим команду
            std::istringstream iss(line);
            std::string cmd; iss >> cmd;

            //Обработка наборов команд...

            //Регистрация
            if (cmd == "REGISTER") {
                std::string u,p; iss >> u >> p;
                bool ok = db->registerUser(u,p);
                sendSSL(clientSock, ok ? "OK REG\n" : "ERROR USER_EXISTS\n");
            }
            //Вход по логину и паролю
            else if (cmd == "LOGIN") {
                std::string u,p; iss >> u >> p;
                int id = db->authenticateUser(u,p);
                if (id > 0) {
                    userId = id;
                    //Сохраняем связь socket->user и обратную
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
            //Список чатов
            else if (cmd == "LIST_CHATS") {
                if (userId < 0) {
                    sendSSL(clientSock, "ERROR NOT_LOGGED\n");
                    continue;
                }

                //Получаем список (chat_id, is_group, chat_name)
                auto chats = db->listUserChats(userId);

                std::ostringstream out;
                out << "CHATS ";
                for (auto &t : chats) {
                    int cid; bool isg; std::string name;
                    std::tie(cid, isg, name) = t; //Берем из кортежа списка чатов
                    out << cid << ":" << (isg ? "1" : "0") << ":" << name << ":";

                    //Добавляем участников списка
                    auto members = db->chatMembers(cid);
                    for (int i = 0; i < members.size(); i++)
                    {
                        if (i != members.size() - 1) out << members[i] << ","; //Участники чата через ,
                        else out << members[i];
                    }
                    out << ";"; //В конце ставим ; в качестве разделителя между чатами
                }

                //Переподписываем клиента на новые chat_id
                {
                    std::lock_guard ukl(subMtx);
                    //Сначала очищаем все старые подписки
                    for (auto &kv : subscribers)
                        kv.second.erase(std::remove(kv.second.begin(),kv.second.end(), clientSock),
                                        kv.second.end());

                    //Затем добавляем в новые
                    for (auto &t : chats) {
                        int cid; bool isg; std::string name;
                        std::tie(cid,isg,name)=t;
                        subscribers[cid].push_back(clientSock);
                    }
                }

                //Формируем и отправляем строку ответа
                std::string res = out.str();
                if (!res.empty()) {
                    res.back() = '\n';  //заменяем последний ';' на '\n'
                }

                sendSSL(clientSock,  res);
            }
            else if (cmd == "CREATE_CHAT") {
                //Создать новый чат (личный или групповой)
                if (userId < 0) {
                    //Если клиент не залогинен — ошибка
                    sendSSL(clientSock, "ERROR NOT_LOGGED\n");
                    continue;
                }

                //Прочитать флаг: 0 = личный, 1 = групповой
                int isGroup;
                iss >> isGroup;

                if (!isGroup) {
                    //Личный чат
                    int peer;
                    iss >> peer;  // ID второго участника

                    //1) Проверка, нет ли уже личного чата между этими двумя пользователями
                    int existing = db->findPrivateChat(userId, peer);
                    if (existing > 0) {
                        //Если чат уже существует — возвращаем ошибку
                        sendSSL(clientSock, "ERROR CHAT_EXISTS\n");
                        continue;
                    }

                    //2) Создаем новый чат без имени
                    int chatId = db->createChat(false, "");
                    //Добавляем обоих пользователей в chat_members
                    db->addUserToChat(chatId, userId);
                    db->addUserToChat(chatId, peer);

                    //3) Уведомляем обоих участников о новом чате (NEW_CHAT)
                    std::string push = "NEW_CHAT\n";
                    std::lock_guard<std::mutex> ul(userMtx);
                    for (int u : {userId, peer}) {
                        for (int s2 : userToSockets[u]) {
                            sendSSL(s2, push);
                        }
                    }

                } else {
                    //Групповой чат

                    //Считываем остаток строки (имя чата + список участников)
                    std::string rest;
                    iss >> std::ws;
                    std::getline(iss, rest);
                    std::istringstream is2(rest);

                    //1) Имя группы
                    std::string gname;
                    is2 >> gname;

                    //2) Состав участников (ID), первый всегда текущий пользователь
                    std::vector<int> members = { userId };
                    int x;
                    while (is2 >> x) {
                        members.push_back(x);
                    }

                    //3) Создаем чат с именем и добавляем всех участников
                    int cid = db->createChat(true, gname);
                    for (int u : members) {
                        db->addUserToChat(cid, u);
                    }

                    //4) Уведомляем всех участников о новом групповом чате
                    std::string push = "NEW_CHAT\n";
                    std::lock_guard<std::mutex> ul(userMtx);
                    for (int u : members) {
                        for (int sock2 : userToSockets[u]) {
                            sendSSL(sock2, push);
                        }
                    }
                }
            }
            else if (cmd == "SEND") {
                //Отправка сообщения в чат
                if (userId < 0) {
                    sendSSL(clientSock, "ERROR NOT_LOGGED\n");
                    continue;
                }
                int cid;
                iss >> cid; //ID чата
                std::string msg;
                std::getline(iss, msg); //Текст сообщения

                //Проверка, что пользователь входит в этот чат
                if (!db->isUserInChat(cid, userId)) {
                    sendSSL(clientSock, "ERROR NO_CHAT_ACCESS\n");
                    continue;
                }

                //Сохраняем сообщение в БД и получаем его msg_id
                int id = db->storeMessage(cid, userId, msg);

                //Отправляем ответ клиенту: OK SENT <msg_id> или ERROR
                std::ostringstream out;
                out << "OK SENT " << id << "\n";
                sendSSL(clientSock, id ? out.str() : "ERROR\n");

                //Если всё успешно, рассылаем другим подписчикам команду NEW_HISTORY
                if (id) {
                    std::lock_guard<std::mutex> sl(subMtx);
                    for (int sock2 : subscribers[cid]) {
                        if (sock2 != clientSock) {
                            sendSSL(sock2, "NEW_HISTORY " + std::to_string(cid) + "\n");
                        }
                    }
                }
            }
            else if (cmd == "HISTORY") {
                //Запрос истории чата (сообщения + события входа/выхода)
                if (userId < 0) {
                    sendSSL(clientSock, "ERROR NOT_LOGGED\n");
                    continue;
                }
                int cid;
                iss >> cid; //ID чата

                //Проверка доступа
                if (!db->isUserInChat(cid, userId)) {
                    sendSSL(clientSock, "ERROR NO_CHAT_ACCESS\n");
                    continue;
                }

                //1) Получаем все сообщения для этого чата
                auto messages = db->getChatHistory(cid, userId);
                //2) Получаем все события
                auto events = db->getChatEvents(cid);

                //Объединяем оба списка по временному штампу
                struct Item {
                    std::string ts; //время и дата
                    enum { MSG, EVT } type;
                    int msg_id;  //только для MSG
                    std::string from; //отправитель или пользователь события
                    std::string text; //текст сообщения или тип события
                };
                std::vector<Item> merged; //итоговый список из сообщений и событий
                merged.reserve(messages.size() + events.size());

                int i = 0, j = 0;
                //Лямбда функция, чтобы быстро получать имя пользователя
                auto userNameById = [&](int uid) {
                    return db->getUsername(uid);
                };

                while (i < messages.size() || j < events.size()) {
                    bool takeMsg = false;
                    if (i < messages.size() && j < events.size()) {
                        //Сравниваем строки формата "YYYY-MM-DD HH:MM", чтобы выбрать нужный порядок сообщений и событий
                        takeMsg = std::get<1>(messages[i]) <= std::get<0>(events[j]);
                    } else {
                        takeMsg = (i < messages.size());
                    }

                    if (takeMsg) {
                        //Добавляем сообщение
                        Item it;
                        it.ts = std::get<1>(messages[i]);
                        it.type = Item::MSG;
                        it.msg_id = std::get<0>(messages[i]);
                        it.from = std::get<2>(messages[i]);
                        it.text = std::get<3>(messages[i]);
                        merged.push_back(std::move(it));
                        ++i;
                    } else {
                        //Добавляем событие
                        Item it;
                        it.ts = std::get<0>(events[j]);
                        it.type = Item::EVT;
                        it.from = userNameById(std::get<1>(events[j]));
                        it.text = (std::get<2>(events[j]) == "LEFT"
                                   ? "покинул(а) чат" : "вошёл в чат");
                        merged.push_back(std::move(it));
                        ++j;
                    }
                }

                //Собираем единый ответ
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

                //Отправляем всю историю одним сообщением
                sendSSL(clientSock, out.str());
            }
            //Удаление сообщения только у себя
            else if (cmd == "DELETE") {
                int msg_id;
                iss >> msg_id;
                //Проверяем, что пользователь — автор сообщения
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
            //Глобальное удаление (для всех)
            else if (cmd == "DELETE_GLOBAL") {
                int msg_id;
                iss >> msg_id;
                int sender = db->getMessageSender(msg_id);
                //Проверяем, что пользователь — автор сообщения
                if (sender != userId) {
                    sendSSL(clientSock, "ERROR NO_RIGHTS\n");
                    continue;
                }

                //Помечаем сообщение как удалённое во всех сессиях
                bool ok = db->deleteMessageGlobal(msg_id);
                if (!ok) {
                    sendSSL(clientSock, "ERROR\n");
                    continue;
                }

                //Уведомляем всех подписчиков чата
                int chat_id = db->getChatIdByMessage(msg_id);
                std::ostringstream notif;
                notif << "MSG_DELETED " << msg_id << "\n";

                //Лочим доступ, так как работаем с общей структурой
                std::lock_guard<std::mutex> lk(subMtx);
                auto it = subscribers.find(chat_id);
                if (it != subscribers.end()) {
                    for (int sock2 : it->second) {
                        sendSSL(sock2, notif.str());
                    }
                }
            }
            //Пользователь покидает групповой чат
            else if (cmd == "LEAVE_CHAT") {
                int cid;
                iss >> cid;
                if (userId < 0 || !db->isUserInChat(cid, userId)) {
                    sendSSL(clientSock, "ERROR\n");
                } else {
                    //Удаляем из участников
                    bool ok = db->removeUserFromChat(cid, userId);
                    sendSSL(clientSock, ok ? "OK LEFT\n" : "ERROR\n");
                    if (ok) {
                        //Формируем уведомление о выходе для других участников
                        std::string name = db->getUsername(userId);
                        auto now = std::chrono::system_clock::now();
                        std::time_t t = std::chrono::system_clock::to_time_t(now);
                        std::tm tm; localtime_r(&t, &tm);
                        char buf[30];
                        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
                        std::ostringstream nt;
                        nt << "USER_LEFT " << cid << " " << name << " " << buf << "\n";

                        //Рассылаем всем остальным участникам
                        std::lock_guard<std::mutex> lk(subMtx);
                        for (int sock2 : subscribers[cid]) {
                            if (sock2 != clientSock) {
                                sendSSL(sock2, nt.str());
                            }
                        }
                        //Убираем клиента из подписчиков
                        auto &vec = subscribers[cid];
                        vec.erase(std::remove(vec.begin(), vec.end(), clientSock), vec.end());
                    }
                }
            }
            //Запрос ID пользователя по имени
            else if (cmd == "GET_USER_ID") {
                std::string nm;
                iss >> nm;
                int uid = db->getUserIdByName(nm);
                sendSSL(clientSock,
                        uid > 0 ? std::to_string(uid) + "\n"
                                : "ERROR NO_SUCH_USER\n");
            }
            //Неизвестная команда
            else {
                sendSSL(clientSock, "ERROR UNKNOWN\n");
            }
        }
    }

    //Клиент отключился — чистим ресурсы
    dropClient(clientSock);

    //Удаляем остатки socket->user
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

int main() {
    //1) Инициализируем SSL
    init_openssl();

    //2) Подключаемся к БД
    db = new Database("host=localhost dbname=chatdb user=chatuser password=123");

    //3) Настраиваем TCP
    serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) {
        perror("socket"); //выводим причину ошибки
        exit(1);
    }

    //Структура с адресом и портом:
    sockaddr_in addr{};
    addr.sin_family = AF_INET; //семейство адресов IPv4
    addr.sin_port = htons(PORT); //порт в сетевом порядке байт (big-endian)
    addr.sin_addr.s_addr = INADDR_ANY; //слушаем на всех локальных интерфейсах (0.0.0.0)

    //Привязываем сокет к адресу/порту:
    if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(serverSock);
        exit(1);
    }

    //Переводим сокет в состояние прослушивания:
    //BACKLOG — максимальная длина очереди входящих подключений
    if (listen(serverSock, BACKLOG) < 0) {
        perror("listen");
        close(serverSock);
        exit(1);
    }

    //Выводим в консоль информацию о том, что сервер готов принимать подключения
    std::cout << "Server listening on port " << PORT << '\n';

    //4) Запускаем админ‑поток для RESET/SHUTDOWN
    std::thread(adminThread).detach();

    //5) Основной цикл: принимаем новые соединения
    while (running) {
        int clientSock = accept(serverSock, nullptr, nullptr);
        if (clientSock < 0) break;
        std::thread(clientHandler, clientSock).detach();
    }

    //6) Чистим ресурсы
    SSL_CTX_free(sslCtx);
    EVP_cleanup();
    delete db;
    return 0;
}
