#include "db.h"
#include <iostream>
#include <sstream>


Database::Database(const std::string& conninfo) {
    conn = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn)!=CONNECTION_OK) {
        std::cerr<<"DB connect error: "<<PQerrorMessage(conn);
        exit(1);
    }
}

Database::~Database() {
    if (conn) PQfinish(conn);
}

bool Database::registerUser(const std::string& username, const std::string& password_hash) {
    std::lock_guard<std::mutex> lk(dbMtx);

    const char* v1[1] = { username.c_str() };
    PGresult* r = PQexecParams(conn,
        "SELECT 1 FROM users WHERE username=$1", 1, nullptr, v1,nullptr,nullptr,0);
    bool exists = PQntuples(r)>0; PQclear(r);
    if (exists) return false;
    const char* v2[2] = { username.c_str(), password_hash.c_str() };
    r = PQexecParams(conn,
        "INSERT INTO users(username,password_hash) VALUES($1,$2)",
        2,nullptr,v2,nullptr,nullptr,0);
    bool ok = (PQresultStatus(r)==PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
}

int Database::authenticateUser(const std::string& username, const std::string& password_hash) {
    std::lock_guard<std::mutex> lk(dbMtx);

    const char* v[2] = { username.c_str(), password_hash.c_str() };
    PGresult* r = PQexecParams(conn,
        "SELECT user_id FROM users WHERE username=$1 AND password_hash=$2",
        2,nullptr,v,nullptr,nullptr,0);
    int id = -1;
    if (PQntuples(r)==1) id = std::stoi(PQgetvalue(r,0,0));
    PQclear(r);
    return id;
}

int Database::findPrivateChat(int u1,int u2) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::string s1=std::to_string(u1), s2=std::to_string(u2);
    const char* v[2]={s1.c_str(),s2.c_str()};
    PGresult* r = PQexecParams(conn,
        R"(SELECT c.chat_id
           FROM chats c
           JOIN chat_members m1 ON c.chat_id=m1.chat_id AND m1.user_id=$1
           JOIN chat_members m2 ON c.chat_id=m2.chat_id AND m2.user_id=$2
           WHERE c.is_group=FALSE
           GROUP BY c.chat_id)",
        2,nullptr,v,nullptr,nullptr,0);
    int id=-1;
    if (PQntuples(r)==1) id=std::stoi(PQgetvalue(r,0,0));
    PQclear(r);
    return id;
}

int Database::createChat(bool is_group, const std::string& chat_name) {
    std::lock_guard<std::mutex> lk(dbMtx);

    if (is_group) {
      const char* v[2] = { "t", chat_name.c_str() };
      PGresult* r = PQexecParams(conn,
          "INSERT INTO chats(is_group,chat_name) VALUES($1,$2) RETURNING chat_id",
          2,nullptr,v,nullptr,nullptr,0);
      int cid=-1;
      if (PQntuples(r)==1) cid=std::stoi(PQgetvalue(r,0,0));
      PQclear(r);
      return cid;
    }
    else {
      const char* v[1] = { "f" };
      PGresult* r = PQexecParams(conn,
          "INSERT INTO chats(is_group,chat_name) VALUES($1,NULL) RETURNING chat_id",
          1,nullptr,v,nullptr,nullptr,0);
      int cid=-1;
      if (PQntuples(r)==1) cid=std::stoi(PQgetvalue(r,0,0));
      PQclear(r);
      return cid;
    }
}

bool Database::addUserToChat(int chat_id,int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::ostringstream a,b; a<<chat_id; b<<user_id;
    const char* v[2]={a.str().c_str(),b.str().c_str()};
    PGresult* r = PQexecParams(conn,
        "INSERT INTO chat_members(chat_id,user_id) VALUES($1,$2)",
        2,nullptr,v,nullptr,nullptr,0);
    bool ok = (PQresultStatus(r)==PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
}

bool Database::isUserInChat(int chat_id,int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::ostringstream a,b; a<<chat_id; b<<user_id;
    const char* v[2]={a.str().c_str(),b.str().c_str()};
    PGresult* r = PQexecParams(conn,
        "SELECT 1 FROM chat_members WHERE chat_id=$1 AND user_id=$2",
        2,nullptr,v,nullptr,nullptr,0);
    bool yes=(PQntuples(r)>0);
    PQclear(r);
    return yes;
}

bool Database::storeMessage(int chat_id,int sender_id,const std::string& content) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::ostringstream a,b; a<<chat_id; b<<sender_id;
    const char* v[3]={a.str().c_str(),b.str().c_str(),content.c_str()};
    PGresult* r=PQexecParams(conn,
        "INSERT INTO messages(chat_id,sender_id,content) VALUES($1,$2,$3)",
        3,nullptr,v,nullptr,nullptr,0);
    bool ok=(PQresultStatus(r)==PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
}

std::vector<std::tuple<std::string,std::string,std::string>>  Database::getChatHistory(int chat_id,int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::string c=std::to_string(chat_id), u=std::to_string(user_id);
    const char* v[2]={c.c_str(),u.c_str()};
    PGresult* r = PQexecParams(conn,
        R"(SELECT to_char(m.created_at,'YYYY-MM-DD HH24:MI') AS ts,
                  u.username, m.content
           FROM messages m
           JOIN users u ON m.sender_id=u.user_id
           LEFT JOIN user_deleted_messages d
             ON d.msg_id=m.msg_id AND d.user_id=$2
           WHERE m.chat_id=$1
             AND NOT m.deleted
             AND d.msg_id IS NULL
           ORDER BY m.created_at)",
        2,nullptr,v,nullptr,nullptr,0);

    std::vector<std::tuple<std::string,std::string,std::string>> out;
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            std::string date = PQgetvalue(r,i,0);
            std::string username = PQgetvalue(r,i,1);
            std::string content = PQgetvalue(r,i,2);
            out.emplace_back(date, username, content);
        }
    } else {
        std::cerr << "listUserChats failed: " << PQerrorMessage(conn);
    }
    PQclear(r);
    return out;
}

int Database::getMessageSender(int msg_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::string s=std::to_string(msg_id);
    const char* v[1]={s.c_str()};
    PGresult* r=PQexecParams(conn,
        "SELECT sender_id FROM messages WHERE msg_id=$1",
        1,nullptr,v,nullptr,nullptr,0);
    int id=-1;
    if (PQntuples(r)==1) id=std::stoi(PQgetvalue(r,0,0));
    PQclear(r);
    return id;
}

bool Database::deleteMessageForUser(int msg_id,int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::ostringstream a,b; a<<msg_id; b<<user_id;
    const char* v[2]={a.str().c_str(),b.str().c_str()};
    PGresult* r=PQexecParams(conn,
      "INSERT INTO user_deleted_messages(msg_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",
      2,nullptr,v,nullptr,nullptr,0);
    bool ok=(PQresultStatus(r)==PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
}

bool Database::deleteMessageGlobal(int msg_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    // Преобразуем msg_id в строку, чтобы получить стабильный указатель на c_str()
    std::string id_str = std::to_string(msg_id);
    const char* vals[1] = { id_str.c_str() };

    // Помечаем сообщение как удалённое для всех (deleted = TRUE)
    PGresult* res = PQexecParams(
        conn,
        "UPDATE messages SET deleted = TRUE WHERE msg_id = $1",
        1,          // число параметров
        nullptr,    // типов данных (используются по умолчанию)
        vals,       // сами значения
        nullptr,
        nullptr,
        0           // формат текстовый
    );
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    PQclear(res);
    return ok;
}

int Database::getUserIdByName(const std::string& username) {
    std::lock_guard<std::mutex> lk(dbMtx);

    const char* v[1]={username.c_str()};
    PGresult* r=PQexecParams(conn,
      "SELECT user_id FROM users WHERE username=$1",
      1,nullptr,v,nullptr,nullptr,0);
    int id=-1;
    if (PQntuples(r)==1) id=std::stoi(PQgetvalue(r,0,0));
    PQclear(r);
    return id;
}

std::vector<std::tuple<int,bool,std::string>> Database::listUserChats(int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    const char* vals[1] = { std::to_string(user_id).c_str() };
    PGresult* res = PQexecParams(conn,
        R"(
            SELECT
              c.chat_id,
              c.is_group,
              c.chat_name
            FROM chats c
            JOIN chat_members m1 ON c.chat_id = m1.chat_id
            WHERE m1.user_id = $1
        )",
        1, nullptr, vals, nullptr, nullptr, 0);

    std::vector<std::tuple<int,bool,std::string>> out;
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            int cid  = std::stoi(PQgetvalue(res,i,0));
            bool isg = (PQgetvalue(res,i,1)[0]=='t');
            std::string name = PQgetvalue(res,i,2);
            out.emplace_back(cid, isg, name);
        }
    } else {
        std::cerr << "listUserChats failed: " << PQerrorMessage(conn);
    }
    PQclear(res);
    return out;
}

std::string Database::getUsername(int user_id) {
    std::lock_guard<std::mutex> lk(dbMtx);

    std::string u=std::to_string(user_id);
    const char* v[1]={u.c_str()};
    PGresult* r=PQexecParams(conn,
      "SELECT username FROM users WHERE user_id=$1",
      1,nullptr,v,nullptr,nullptr,0);
    std::string nm;
    if (PQntuples(r)==1) nm=PQgetvalue(r,0,0);
    PQclear(r);
    return nm;
}

bool Database::deleteEverything() {
  std::lock_guard<std::mutex> lk(dbMtx);

  const char* v[0]={};
  PGresult* r=PQexecParams(conn,
      "DELETE FROM users",
      0,nullptr,v,nullptr,nullptr,0);
    r=PQexecParams(conn,
      "DELETE FROM chats",
      0,nullptr,v,nullptr,nullptr,0);
    r=PQexecParams(conn,
      "DELETE FROM chat_members",
      0,nullptr,v,nullptr,nullptr,0);
    r=PQexecParams(conn,
      "DELETE FROM messages", 
      0,nullptr,v,nullptr,nullptr,0);
    r=PQexecParams(conn,
      "DELETE FROM user_deleted_messages",
      0,nullptr,v,nullptr,nullptr,0);
    bool ok = (PQresultStatus(r) == PGRES_COMMAND_OK);
    PQclear(r);
    return ok;
}

std::vector<std::string> Database::chatMembers(int chat_id) {
    std::lock_guard<std::mutex> lk(dbMtx);
    std::string c = std::to_string(chat_id);
    const char* v[1] = { c.c_str() };
    PGresult* r = PQexecParams(conn,
      R"(SELECT u.username
           FROM users u
           JOIN chat_members m ON m.user_id=u.user_id
          WHERE m.chat_id=$1
        )",
      1, nullptr, v, nullptr, nullptr, 0);
    std::vector<std::string> out;
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        out.reserve(n);
        for (int i = 0; i < n; ++i)
            out.emplace_back(PQgetvalue(r,i,0));
    }
    PQclear(r);
    return out;
}