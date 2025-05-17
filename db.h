#pragma once
#include <string>
#include <vector>
#include <tuple>
#include <utility>
#include <mutex>
#include <postgresql/libpq-fe.h>

class Database {
public:
    Database(const std::string& conninfo);
    ~Database();

    bool registerUser(const std::string& username, const std::string& password_hash);
    int  authenticateUser(const std::string& username, const std::string& password_hash);

    // Ищет существующий приватный чат
    int  findPrivateChat(int u1, int u2);

    // Создаёт чат (group→chat_name, otherwise "")
    int  createChat(bool is_group, const std::string& chat_name);
    bool addUserToChat(int chat_id, int user_id);
    bool isUserInChat(int chat_id, int user_id);

    bool storeMessage(int chat_id, int sender_id, const std::string& content);

    // Теперь принимает user_id, чтобы отфильтровать «удалённое для себя»
    std::vector<std::tuple<std::string,std::string,std::string>>  getChatHistory(int chat_id, int user_id);

    // Удалить глобально (для всех) — должен вызывать только автор
    bool deleteMessageGlobal(int msg_id);
    // Удалить только для себя
    bool deleteMessageForUser(int msg_id, int user_id);

    std::vector<std::string> chatMembers(int chat_id);
    std::vector<std::tuple<int,bool,std::string>> listUserChats(int user_id);
    int  getUserIdByName(const std::string& username);
    std::string getUsername(int user_id);

    /// Возвращает user_id автора сообщения msg_id
    int    getMessageSender(int msg_id);

    /// Возвращает chat_id, в котором было msg_id
    int    getChatIdOfMessage(int msg_id);

    bool deleteEverything();
private:
    PGconn* conn;

    std::mutex dbMtx;
};
