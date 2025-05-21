#include "db.h"
#include <iostream>
#include <sstream>


Database::Database(const std::string& conninfo) {
  //открываем соединение с базой
  conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "Ошибка подключения к БД: " << PQerrorMessage(conn);
    std::exit(1);
  }
}

Database::~Database() {
  //закрываем соединение, если оно было открыто
  if (conn) {
    PQfinish(conn);
    conn = nullptr;
  }
}

bool Database::registerUser(const std::string& username, const std::string& password_hash) {
  //блокируем на время работы с БД
  std::lock_guard<std::mutex> lock(dbMtx);

  //1) проверяем, что пользователя с таким именем ещё нет
  {
    const char* params[] = { username.c_str() };
    PGresult* res = PQexecParams(
      conn,
      "SELECT 1 FROM users WHERE username = $1",
      1, nullptr, params, nullptr, nullptr, 0
    );
    bool exists = (PQntuples(res) > 0);
    PQclear(res);
    if (exists) {
      //уже есть такой пользователь
      return false;
    }
  }

  //2) вставляем нового пользователя
  {
    const char* params[] = {
      username.c_str(),
      password_hash.c_str()
    };
    PGresult* res = PQexecParams(
      conn,
      "INSERT INTO users(username, password_hash) VALUES($1, $2)",
      2, nullptr, params, nullptr, nullptr, 0
    );
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
  }
}

int Database::authenticateUser(const std::string& username, const std::string& password) {
  std::lock_guard<std::mutex> lk(dbMtx);

  const char* v[2] = { 
    username.c_str(), 
    password.c_str() 
  };
  PGresult* r = PQexecParams(conn,
    "SELECT user_id FROM users WHERE username=$1 AND password_hash=$2",
    2,nullptr,v,nullptr,nullptr,0);

  int id = -1;
  if (PQntuples(r)==1) 
    id = std::stoi(PQgetvalue(r,0,0)); //берем id, преобразуем в int

  PQclear(r);
  return id;
}

int Database::findPrivateChat(int u1,int u2) {
  std::lock_guard<std::mutex> lock(dbMtx);

  //параметры to_string чтобы получить const char*
  std::string s1 = std::to_string(u1);
  std::string s2 = std::to_string(u2);
  const char* params[] = { s1.c_str(), s2.c_str() };

  //ищем чат, в котором оба пользователя и is_group = false
  PGresult* res = PQexecParams(
    conn,
    R"(
      SELECT c.chat_id
        FROM chats c
        JOIN chat_members m1 ON c.chat_id = m1.chat_id AND m1.user_id = $1
        JOIN chat_members m2 ON c.chat_id = m2.chat_id AND m2.user_id = $2
      WHERE c.is_group = FALSE
      GROUP BY c.chat_id
    )",
      2, nullptr, params, nullptr, nullptr, 0
    );

    int chatId = -1;
    if (PQntuples(res) == 1)
        chatId = std::stoi(PQgetvalue(res, 0, 0));

    PQclear(res);
    return chatId;
}

int Database::createChat(bool is_group, const std::string& chat_name) {
  std::lock_guard<std::mutex> lock(dbMtx);

  if (is_group) {
    //создаём групповой чат с названием группы
    const char* params[] = { "t", chat_name.c_str() };
    PGresult* res = PQexecParams(
      conn,
        "INSERT INTO chats(is_group, chat_name) VALUES($1, $2) RETURNING chat_id",
          2, nullptr, params, nullptr, nullptr, 0
        );
    int cid = -1;
    if (PQntuples(res) == 1)
      cid = std::stoi(PQgetvalue(res, 0, 0));
      
    PQclear(res);
    return cid;
  } else {
    //создаём приватный чат без имени
    const char* params[] = { "f" };
    PGresult* res = PQexecParams(
      conn,
       "INSERT INTO chats(is_group, chat_name) VALUES($1, NULL) RETURNING chat_id",
          1, nullptr, params, nullptr, nullptr, 0
        );
    int cid = -1;
    if (PQntuples(res) == 1)
            cid = std::stoi(PQgetvalue(res, 0, 0));

    PQclear(res);
    return cid;
  }
}

bool Database::addUserToChat(int chat_id,int user_id) {
  std::lock_guard<std::mutex> lock(dbMtx);

  //формируем параметры
  std::ostringstream a, b;
  a << chat_id; b << user_id;
  const char* params[] = { a.str().c_str(), b.str().c_str() };

  //добавляем в chat_members
  PGresult* res = PQexecParams(
    conn,
      "INSERT INTO chat_members(chat_id, user_id) VALUES($1, $2)",
        2, nullptr, params, nullptr, nullptr, 0
    );
  bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
  PQclear(res);
  return ok;
}

bool Database::isUserInChat(int chat_id,int user_id) {
  std::lock_guard<std::mutex> lock(dbMtx);

  std::ostringstream a, b;
  a << chat_id; b << user_id;
  const char* params[] = { a.str().c_str(), b.str().c_str() };

  //проверяем наличие в chat_members
  PGresult* res = PQexecParams(
    conn,
      "SELECT 1 FROM chat_members WHERE chat_id = $1 AND user_id = $2",
        2, nullptr, params, nullptr, nullptr, 0
    );
  bool found = (PQntuples(res) > 0);
  PQclear(res);
  return found;
}

int Database::storeMessage(int chat_id,int sender_id,const std::string& content) {
  std::lock_guard<std::mutex> lock(dbMtx);

  std::ostringstream a, b;
  a << chat_id; b << sender_id;
  const char* params[] = {
    a.str().c_str(),
    b.str().c_str(),
    content.c_str()
  };

  //возвращает msg_id после вставки
  PGresult* res = PQexecParams(
    conn,
      "INSERT INTO messages(chat_id, sender_id, content) "
      "VALUES($1, $2, $3) RETURNING msg_id",
        3, nullptr, params, nullptr, nullptr, 0
    );

  int msgId = -1;
  if (PQntuples(res) == 1)
        msgId = std::stoi(PQgetvalue(res, 0, 0));
  
  PQclear(res);
  return msgId;
}

std::vector<std::tuple<int, std::string,std::string,std::string>>  Database::getChatHistory(int chat_id,int user_id) {
  std::lock_guard<std::mutex> lock(dbMtx);

  //параметры chat_id и user_id
  std::string c = std::to_string(chat_id);
  std::string u = std::to_string(user_id);
  const char* params[] = { c.c_str(), u.c_str() };

  //выбираем все сообщения, не удалённые и не помеченные в user_deleted_messages

  //используем LEFT JOIN потому что нам нужно выбрать все сообщения из чата, 
  //даже те, для которых в таблице user_deleted_messages нет записи (то есть их пользователь не удалял)
  PGresult* res = PQexecParams(
    conn,
    R"(
      SELECT
        m.msg_id,
        to_char(m.created_at,'YYYY-MM-DD HH24:MI') AS ts,
        u.username,
        m.content
      FROM messages m
      JOIN users u 
        ON m.sender_id = u.user_id
      LEFT JOIN user_deleted_messages d 
        ON d.msg_id = m.msg_id AND d.user_id = $2
      WHERE m.chat_id = $1
        AND NOT m.deleted
        AND d.msg_id IS NULL
      ORDER BY m.created_at
    )",
      2, nullptr, params, nullptr, nullptr, 0
  );

  //формируем вектор записей об истории
  std::vector<std::tuple<int,std::string,std::string,std::string>> out;
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int rows = PQntuples(res);
    out.reserve(rows);
    for (int i = 0; i < rows; ++i) {
      int mid = std::stoi(PQgetvalue(res, i, 0));
      std::string ts = PQgetvalue(res, i, 1);
      std::string usr = PQgetvalue(res, i, 2);
      std::string msg = PQgetvalue(res, i, 3);
      out.emplace_back(mid, ts, usr, msg);
    }
  } 
  
  PQclear(res);
  return out;
}

int Database::getMessageSender(int msg_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  std::string s = std::to_string(msg_id);
  const char* v[1] = {s.c_str()};

  //очень простой запрос из одной таблицы
  PGresult* r = PQexecParams(conn,
    "SELECT sender_id FROM messages WHERE msg_id=$1",
      1,nullptr,v,nullptr,nullptr,0
    );

  int id = -1;
  if (PQntuples(r)==1) 
    id = std::stoi(PQgetvalue(r,0,0));

  PQclear(r);
  return id;
}

bool Database::deleteMessageForUser(int msg_id,int user_id) {
  std::lock_guard<std::mutex> lock(dbMtx);

  std::ostringstream a, b;
  a << msg_id; b << user_id;
  const char* params[] = { a.str().c_str(), b.str().c_str() };

  //помечаем сообщение как удалённое для данного user_id
  PGresult* res = PQexecParams(
    conn,
      R"(INSERT INTO user_deleted_messages(msg_id, user_id)
      VALUES($1, $2) ON CONFLICT DO NOTHING)", //не ломаем сервер при попытке дважды удалить одно и то же сообщение
        2, nullptr, params, nullptr, nullptr, 0
    );

  bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
  PQclear(res);
  return ok;
}

bool Database::deleteMessageGlobal(int msg_id) {
  std::lock_guard<std::mutex> lock(dbMtx);

  std::string id_str = std::to_string(msg_id);
  const char* params[] = { id_str.c_str() };

  //устанавливаем deleted = TRUE
  PGresult* res = PQexecParams(
    conn,
      "UPDATE messages SET deleted = TRUE WHERE msg_id = $1",
        1, nullptr, params, nullptr, nullptr, 0
    );
  
  bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
  PQclear(res);
  return ok;
}

int Database::getUserIdByName(const std::string& username) {
  std::lock_guard<std::mutex> lk(dbMtx);

  const char* v[1] = {username.c_str()};

  PGresult* r = PQexecParams(conn,
    "SELECT user_id FROM users WHERE username=$1",
      1,nullptr,v,nullptr,nullptr,0);

  int id = -1;
  if (PQntuples(r) == 1) 
    id = std::stoi(PQgetvalue(r,0,0));

  PQclear(r);
  return id;
}

std::vector<std::tuple<int,bool,std::string>> Database::listUserChats(int user_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  //передаём user_id как строку
  std::string uidStr = std::to_string(user_id);
  const char* params[1] = { uidStr.c_str() };

  //выбираем id, признак группового чата и имя чата для данного пользователя
  PGresult* res = PQexecParams(
    conn,
    R"(
      SELECT c.chat_id, c.is_group, c.chat_name
        FROM chats c
        JOIN chat_members m ON c.chat_id = m.chat_id
      WHERE m.user_id = $1
      )",
        1, nullptr, params, nullptr, nullptr, 0
    );


  //формируем соответствующий вектор кортежей из списка чатов
  std::vector<std::tuple<int,bool,std::string>> out;
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int rowCount = PQntuples(res);
    out.reserve(rowCount);
    for (int i = 0; i < rowCount; ++i) {
      int chatId = std::stoi(PQgetvalue(res, i, 0));
      bool isGroup = (PQgetvalue(res, i, 1)[0] == 't');
      std::string name = PQgetvalue(res, i, 2);
      out.emplace_back(chatId, isGroup, name);
    }
  }

  PQclear(res);
  return out;
}

std::string Database::getUsername(int user_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  std::string u = std::to_string(user_id);
  const char* v[1] = {u.c_str()};
  PGresult* r = PQexecParams(conn,
      "SELECT username FROM users WHERE user_id=$1",
      1,nullptr,v,nullptr,nullptr,0);

  std::string name;
  if (PQntuples(r)==1) 
    name = PQgetvalue(r,0,0);

  PQclear(r);
  return name;
}

bool Database::deleteEverything() {
  std::lock_guard<std::mutex> lk(dbMtx);

  const char* noParams[0] = {};

  //Удаляем данные из всех таблиц
  PQexecParams(conn, "DELETE FROM users", 0, nullptr, noParams, nullptr, nullptr, 0);
  PQexecParams(conn, "DELETE FROM chats", 0, nullptr, noParams, nullptr, nullptr, 0);
  PQexecParams(conn, "DELETE FROM chat_members", 0, nullptr, noParams, nullptr, nullptr, 0);
  PQexecParams(conn, "DELETE FROM messages", 0, nullptr, noParams, nullptr, nullptr, 0);
  PQexecParams(conn, "DELETE FROM user_deleted_messages", 0, nullptr, noParams, nullptr, nullptr, 0);
  PGresult* res = PQexecParams(conn, "DELETE FROM chat_events", 0, nullptr, noParams, nullptr, nullptr, 0);

  bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
  PQclear(res);
  return success;
}

std::vector<std::string> Database::chatMembers(int chat_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  std::string cidStr = std::to_string(chat_id);
  const char* params[1] = { cidStr.c_str() };

  PGresult* res = PQexecParams(
    conn,
      R"(
        SELECT u.username
        FROM users u
          JOIN chat_members m ON u.user_id = m.user_id
        WHERE m.chat_id = $1
      )",
        1, nullptr, params, nullptr, nullptr, 0
    );

  std::vector<std::string> members;
  if (PQresultStatus(res) == PGRES_TUPLES_OK) {
    int rowCount = PQntuples(res);
    members.reserve(rowCount);
    for (int i = 0; i < rowCount; ++i) {
      members.emplace_back(PQgetvalue(res, i, 0));
    }
  }

  PQclear(res);
  return members;
}

int Database::getChatIdByMessage(int msg_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  std::string midStr = std::to_string(msg_id);
  const char* params[1] = { midStr.c_str() };

  PGresult* res = PQexecParams(
    conn,
      "SELECT chat_id FROM messages WHERE msg_id=$1",
        1, nullptr, params, nullptr, nullptr, 0
    );

  int chatId = -1;
  if (PQntuples(res) == 1)
    chatId = std::stoi(PQgetvalue(res, 0, 0));

  PQclear(res);
  return chatId;
}

bool Database::removeUserFromChat(int chat_id, int user_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  //1) Удаляем из chat_members
  {
    std::ostringstream sc, su;
    sc << chat_id;
    su << user_id;
    const char* params1[2] = { sc.str().c_str(), su.str().c_str() };
    PGresult* r = PQexecParams(
      conn,
        "DELETE FROM chat_members WHERE chat_id=$1 AND user_id=$2",
          2, nullptr, params1, nullptr, nullptr, 0
      );
    PQclear(r);
  }

  //2) Вставляем запись в chat_events с типом LEFT (покинул чат)
  {
    std::ostringstream sc, su;
    sc << chat_id;
    su << user_id;
    const char* params2[3] = { sc.str().c_str(), su.str().c_str(), "LEFT" };
    PGresult* r = PQexecParams(
      conn,
        R"(
          INSERT INTO chat_events(chat_id, user_id, event_type, event_ts)
          VALUES($1, $2, $3, now())
        )",
          3, nullptr, params2, nullptr, nullptr, 0
      );
    
    bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
  }
}

std::vector<std::tuple<std::string,int,std::string>> Database::getChatEvents(int chat_id) {
  std::lock_guard<std::mutex> lk(dbMtx);

  std::string cidStr = std::to_string(chat_id);
  const char* params[1] = { cidStr.c_str() };

  //выбираем timestamp (дату и время), user_id, тип события
  PGresult* res = PQexecParams(
    conn,
    R"(
      SELECT
        to_char(event_ts,'YYYY-MM-DD HH24:MI'),
        user_id,
        event_type
      FROM chat_events
      WHERE chat_id=$1
      ORDER BY event_ts
    )",
      1, nullptr, params, nullptr, nullptr, 0
    );

  std::vector<std::tuple<std::string,int,std::string>> events;
  int rowCount = PQntuples(res);
  events.reserve(rowCount);

  for (int i = 0; i < rowCount; ++i) {
    std::string ts = PQgetvalue(res, i, 0);
    int uid = std::stoi(PQgetvalue(res, i, 1));
    std::string type = PQgetvalue(res, i, 2);
    events.emplace_back(ts, uid, type);
  }

  PQclear(res);
  return events;
}