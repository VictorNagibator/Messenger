#ifndef DB_H
#define DB_H

#include <string>
#include <vector>
#include <postgresql/libpq-fe.h>

class Database {
public:
    Database(const std::string& conninfo);
    ~Database();

    // Пользователи
    bool registerUser(const std::string& username, const std::string& password_hash);
    int  authenticateUser(const std::string& username, const std::string& password_hash);

    // Чаты
    int  createChat(bool is_group, const std::string& chat_name);
    bool addUserToChat(int chat_id, int user_id);
    bool isUserInChat(int chat_id, int user_id);

    // Сообщения
    bool storeMessage(int chat_id, int sender_id, const std::string& content);
    std::string getChatHistory(int chat_id);
    bool deleteMessage(int msg_id);

    std::vector<std::pair<int,std::string>> listUsers();
private:
    PGconn* conn;
};

#endif // DB_H