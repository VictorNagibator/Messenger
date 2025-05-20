#pragma once

#include <string>
#include <vector>
#include <tuple>
#include <mutex>
#include <postgresql/libpq-fe.h>

//Класс для работы с базой PostgreSQL — регистрация, чаты, сообщения, события
class Database {
public:
    //Подключается к БД по строке соединения
    //пример: "host=... dbname=... user=... password=..."
    explicit Database(const std::string& conninfo);
    //explicit для того, чтобы не было неявного преобразования из string

    //Закрывает соединение с БД
    ~Database();

    //Регистрирует нового пользователя
    bool registerUser(const std::string& username,
                      const std::string& password);

    //Проверяет логин/пароль
    //user_id при успехе или -1 при ошибке
    int authenticateUser(const std::string& username,
                         const std::string& password_hash);

    //Ищет приватный чат между двумя пользователями.
    //return chat_id, или -1 если чат не найден.
    int findPrivateChat(int user1, int user2);

    //Создаёт чат
    //новый chat_id или -1 при ошибке
    int createChat(bool is_group, const std::string& chat_name);

    //Добавляет пользователя в чат
    bool addUserToChat(int chat_id, int user_id);

    //Проверяет, состоит ли пользователь в чате
    bool isUserInChat(int chat_id, int user_id);

    //Сохраняет новое сообщение
    //Возвращает сгенерированный msg_id или -1 при ошибке
    int storeMessage(int chat_id,
                     int sender_id,
                     const std::string& content);

    //Возвращает историю чата с фильтрацией по удалённым сообщениям для данного user_id
    //Возвращает вектор кортежей (msg_id, "YYYY-MM-DD HH:MM", username, content)
    std::vector<std::tuple<int, std::string, std::string, std::string>>
        getChatHistory(int chat_id, int user_id);

    //Помечает сообщение глобально удалённым, вызывать может только автор
    bool deleteMessageGlobal(int msg_id);

    //Добавляет в user_deleted_messages, чтобы скрыть у одного пользователя (автора)
    bool deleteMessageForUser(int msg_id, int user_id);

    //Список имён всех участников чата
    std::vector<std::string> chatMembers(int chat_id);

    //Список чатов, в которых участвует пользователь
    //Возвращает вектор (chat_id, is_group, chat_name)
    std::vector<std::tuple<int, bool, std::string>> listUserChats(int user_id);

    //Возвращает user_id по его имени, или -1 если не найден
    int getUserIdByName(const std::string& username);

    //Возвращает имя пользователя по user_id, или пустую строку
    std::string getUsername(int user_id);

    //Автор сообщения
    int getMessageSender(int msg_id);

    //Определяет, в каком чате было сообщение.
    int getChatIdByMessage(int msg_id);

    //Удаляет пользователя из чата и фиксирует событие "LEFT"
    bool removeUserFromChat(int chat_id, int user_id);

    //Возвращает список событий чата
    //вектор ("YYYY-MM-DD HH:MM", user_id, event_type)
    std::vector<std::tuple<std::string, int, std::string>>
        getChatEvents(int chat_id);

    //Полностью очищает все таблицы (для админских целей)
    bool deleteEverything();

private:
    PGconn* conn; //Компонент подключения libpq
    std::mutex dbMtx; //Защищает доступ к conn и всем методам (иначе крашится сервер при одновременных попытках подключения пользователей)
};