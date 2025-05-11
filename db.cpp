#include "db.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <utility>

Database::Database(const std::string& conninfo) {
    conn = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connect error: " << PQerrorMessage(conn);
        exit(1);
    }
}

Database::~Database() {
    if (conn) PQfinish(conn);
}

bool Database::registerUser(const std::string& username, const std::string& password_hash) {
    // Проверяем, есть ли уже такой логин
    const char* chkVals[1] = { username.c_str() };
    PGresult* chk = PQexecParams(conn,
        "SELECT 1 FROM users WHERE username = $1",
        1, nullptr, chkVals, nullptr, nullptr, 0);
    bool exists = (PQntuples(chk) > 0);
    PQclear(chk);

    if (exists) {
        // уже есть
        return false;
    }
    // вставляем нового
    const char* insVals[2] = { username.c_str(), password_hash.c_str() };
    PGresult* res = PQexecParams(conn,
        "INSERT INTO users(username,password_hash) VALUES($1,$2)",
        2, nullptr, insVals, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::cerr << "registerUser error: " << PQerrorMessage(conn);
    }
    PQclear(res);
    return ok;
}

int Database::authenticateUser(const std::string& username, const std::string& password_hash) {
    const char* vals[2] = { username.c_str(), password_hash.c_str() };
    PGresult* res = PQexecParams(conn,
        "SELECT user_id FROM users WHERE username=$1 AND password_hash=$2",
        2, nullptr, vals, nullptr, nullptr, 0);
    int id = -1;
    if (PQresultStatus(res)==PGRES_TUPLES_OK && PQntuples(res)==1) {
        id = std::stoi(PQgetvalue(res,0,0));
    }
    PQclear(res);
    return id;
}

int Database::createChat(bool is_group, const std::string& chat_name) {
    const char* vals[2] = { is_group ? "t" : "f", chat_name.c_str() };
    PGresult* res = PQexecParams(conn,
        "INSERT INTO chats(is_group,chat_name) VALUES($1,$2) RETURNING chat_id",
        2,nullptr,vals,nullptr,nullptr,0);
    int chat_id = -1;
    if (PQresultStatus(res)==PGRES_TUPLES_OK) {
        chat_id = std::stoi(PQgetvalue(res,0,0));
    }
    PQclear(res);
    return chat_id;
}

bool Database::addUserToChat(int chat_id, int user_id) {
    std::ostringstream q; q<<chat_id; std::string s1=q.str();
    std::ostringstream r; r<<user_id; std::string s2=r.str();
    const char* vals[2] = { s1.c_str(), s2.c_str() };
    PGresult* res = PQexecParams(conn,
        "INSERT INTO chat_members(chat_id,user_id) VALUES($1,$2) ON CONFLICT DO NOTHING",
        2,nullptr,vals,nullptr,nullptr,0);
    bool ok = PQresultStatus(res)==PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

bool Database::isUserInChat(int chat_id, int user_id) {
    std::ostringstream q; q<<chat_id; std::string s1=q.str();
    std::ostringstream r; r<<user_id; std::string s2=r.str();
    const char* vals[2] = { s1.c_str(), s2.c_str() };
    PGresult* res = PQexecParams(conn,
        "SELECT 1 FROM chat_members WHERE chat_id=$1 AND user_id=$2",
        2,nullptr,vals,nullptr,nullptr,0);
    bool yes = (PQntuples(res)==1);
    PQclear(res);
    return yes;
}

bool Database::storeMessage(int chat_id, int sender_id, const std::string& content) {
    std::ostringstream a,b;
    a<<chat_id; b<<sender_id;
    const char* vals[3] = { a.str().c_str(), b.str().c_str(), content.c_str() };
    PGresult* res = PQexecParams(conn,
        "INSERT INTO messages(chat_id,sender_id,content) VALUES($1,$2,$3)",
        3,nullptr,vals,nullptr,nullptr,0);
    bool ok = PQresultStatus(res)==PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

std::string Database::getChatHistory(int chat_id) {
    std::ostringstream q; q<<chat_id;
    const char* vals[1] = { q.str().c_str() };
    PGresult* res = PQexecParams(conn,
        "SELECT msg_id,sender_id,content,created_at,deleted "
        "FROM messages WHERE chat_id=$1 ORDER BY created_at ASC",
        1,nullptr,vals,nullptr,nullptr,0);
    std::ostringstream out;
    if (PQresultStatus(res)==PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        for (int i=0;i<n;i++) {
            std::string msgid = PQgetvalue(res,i,0);
            std::string sid   = PQgetvalue(res,i,1);
            std::string txt   = PQgetvalue(res,i,2);
            std::string ts    = PQgetvalue(res,i,3);
            std::string del   = PQgetvalue(res,i,4);
            out<<"["<<msgid<<"] ["<<ts<<"] User#"<<sid<<": "<<txt;
            if (del=="t") out<<" (deleted)";
            out<<"\n";
        }
    }
    PQclear(res);
    return out.str();
}

bool Database::deleteMessage(int msg_id) {
    std::ostringstream q; q<<msg_id;
    const char* vals[1] = { q.str().c_str() };
    PGresult* res = PQexecParams(conn,
        "UPDATE messages SET deleted=TRUE WHERE msg_id=$1",
        1,nullptr,vals,nullptr,nullptr,0);
    bool ok = PQresultStatus(res)==PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

std::vector<std::pair<int,std::string>> Database::listUsers() {
    PGresult* res = PQexec(conn,
      "SELECT user_id, username FROM users ORDER BY user_id");
    std::vector<std::pair<int,std::string>> out;
    if (PQresultStatus(res)==PGRES_TUPLES_OK) {
        int n = PQntuples(res);
        for (int i = 0; i < n; ++i) {
            int id = std::stoi(PQgetvalue(res,i,0));
            std::string name = PQgetvalue(res,i,1);
            out.emplace_back(id,name);
        }
    }
    PQclear(res);
    return out;
}