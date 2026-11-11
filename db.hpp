#pragma once
#include "util.hpp"
#include <cstdio>
#include <jsoncpp/json/forwards.h>
#include <jsoncpp/json/value.h>
#include <mutex>
#include <mysql/mysql.h>

class user_table
{
private:
    MYSQL *m_mysql; // mysql操作句柄
    std::mutex m_mutex; // 互斥锁保护数据库的访问操作
    // 保存连接参数,用于连接失效后重新建立连接(MySQL wait_timeout 到期会关闭空闲连接)
    std::string m_host;
    std::string m_user;
    std::string m_pass;
    std::string m_dbname;
    uint16_t m_port;
public:
    // 移到类外或文件顶部
    inline static const char* INSERT_USER = "insert user values (null, '%s', SHA2('%s', 256), 1000, 0, 0);";
    inline static const char* LOGIN_USER = "select id, score, total_count, win_count from user where username = '%s' and password = SHA2('%s', 256);";
    inline static const char* USER_BY_NAME = "select id, score, total_count, win_count from user where username = '%s';";
    inline static const char* USER_BY_ID = "select username, score, total_count, win_count from user where id = %d;";
    inline static const char* USER_WIN = "update user set score = score + 30, total_count = total_count + 1, win_count = win_count + 1 where id = %d;";
    inline static const char* USER_LOSE = "update user set score = score - 30, total_count = total_count + 1 where id = %d;";
    user_table(const std::string& host,
                               const std::string& username,
                               const std::string& password,
                               const std::string& dbname, uint16_t port = 3306) 
    :m_host(host)
    ,m_user(username)
    ,m_pass(password)
    ,m_dbname(dbname)
    ,m_port(port)
    {
        m_mysql = mysql_util::mysql_create(m_host, m_user, m_pass, m_dbname, m_port);
        assert(m_mysql != nullptr);
    }

    ~user_table() 
    {
        mysql_util::mysql_destroy(m_mysql);
    }

private:
    // 确保数据库连接可用:连接空闲超过 MySQL 的 wait_timeout(默认8小时)后会被
    // 服务端单方面关闭,若不做处理,之后所有 SQL 都会以"Lost connection / client was
    // disconnected by the server because of inactivity"永久失败,只能重启进程恢复。
    // 这里在执行任何 SQL 之前先 ping 一次,发现连接失效就自动重连。
    // 注意:必须在持有 m_mutex 的情况下调用。
    bool ensure_connection()
    {
        if (m_mysql != nullptr && mysql_ping(m_mysql) == 0)
            return true;
        // 连接已失效(或尚未建立),关闭旧句柄后重新连接
        if (m_mysql != nullptr)
            ELOG("mysql connection lost, reconnecting : %s", mysql_error(m_mysql));
        mysql_util::mysql_destroy(m_mysql);
        m_mysql = mysql_util::mysql_create(m_host, m_user, m_pass, m_dbname, m_port);
        if (m_mysql == nullptr)
        {
            ELOG("mysql reconnect failed!");
            return false;
        }
        DLOG("mysql reconnected successfully!");
        return true;
    }

    // 安全地把结果集字段转换为 int:字段可能为 NULL(mysql_fetch_row 返回 nullptr),
    // 且 std::stoi 遇到空串/非法字符串会抛异常;统一兜底,避免异常冒泡导致进程崩溃
    static int safe_stoi(const char *str, int def = 0)
    {
        if (str == nullptr)
            return def;
        try
        {
            return std::stoi(str);
        }
        catch (const std::exception &e)
        {
            ELOG("safe_stoi failed : %s", e.what());
            return def;
        }
    }

public:
    // 将用户输入转义为可安全拼入SQL的字符串(防SQL注入),同时校验长度
    // 返回false表示输入非法(空或超长)
    bool escape_field(const char *src, char *dst, size_t dst_size, size_t max_len)
    {
        if (src == nullptr)
            return false;
        size_t len = strlen(src);
        if (len == 0 || len > max_len)
            return false;
        // 转义后的最长长度为 len*2+1
        if (dst_size < len * 2 + 1)
            return false;
        mysql_real_escape_string(m_mysql, dst, src, len);
        return true;
    }
    bool insert(Json::Value &user) // 注册时新增用户
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("insert: database unavailable!");
            return false;
        }
        if (user["password"].isNull() || user["username"].isNull())
        {
            DLOG("insert: username or password is empty!");
            return false;
        }
        // 校验长度并转义,防止SQL注入
        char ubuf[128] = { 0 };
        char pbuf[192] = { 0 };
        if (escape_field(user["username"].asCString(), ubuf, sizeof(ubuf), 32) == false ||
            escape_field(user["password"].asCString(), pbuf, sizeof(pbuf), 64) == false)
        {
            DLOG("insert: username or password invalid!");
            return false;
        }
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), INSERT_USER, ubuf, pbuf);
        bool ret2 = mysql_util::mysql_exec(m_mysql, sql);
        if (ret2 == false)
        {
            ELOG("insert: SQL execution failed!");
            return false;
        }

        return true;
    }

    bool login(Json::Value &user) // 登录验证，并返回用户的详细信息
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("login: database unavailable!");
            return false;
        }
        if (user["password"].isNull() || user["username"].isNull())
        {
            DLOG("login: username or password is empty!");
            return false;
        }
        // 校验长度并转义,防止SQL注入
        char ubuf[128] = { 0 };
        char pbuf[192] = { 0 };
        if (escape_field(user["username"].asCString(), ubuf, sizeof(ubuf), 32) == false ||
            escape_field(user["password"].asCString(), pbuf, sizeof(pbuf), 64) == false)
        {
            DLOG("login: username or password invalid!");
            return false;
        }
        // 以用户名和密码作为共同查询条件 查询到数据则表示用户名密码一致，没有信息则用户名密码错误
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), LOGIN_USER, ubuf, pbuf);
        bool ret = mysql_util::mysql_exec(m_mysql, sql);
        if (ret == false)
        {
            DLOG("login: SQL execution failed!");
            return false;
        }
        // 要么有数据，要么没有数据，有数据也只能有一条数据
        MYSQL_RES *res = mysql_store_result(m_mysql);
        if (res == nullptr)
        {
            DLOG("login: failed to retrieve result!");
            mysql_free_result(res);
            return false;
        }
        int row_num = mysql_num_rows(res);
        if (row_num == 0)
        {
            DLOG("login: wrong username or password!");
            mysql_free_result(res);
            return false;
        }
        if (row_num > 1)
        {
            DLOG("login: duplicate user records found!");
            mysql_free_result(res);
            return false;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        user["id"] = safe_stoi(row[0]);
        user["score"] = safe_stoi(row[1]);
        user["total_count"] = safe_stoi(row[2]);
        user["win_count"] = safe_stoi(row[3]);

        mysql_free_result(res);
        return true;
    }

    bool select_by_name(const std::string &name, Json::Value &user) // 通过用户名获取用户信息
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("select by name: database unavailable!");
            return false;
        }
        // 校验长度并转义,防止SQL注入
        char ubuf[128] = { 0 };
        if (escape_field(name.c_str(), ubuf, sizeof(ubuf), 32) == false)
        {
            DLOG("select by name: username invalid!");
            return false;
        }
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), USER_BY_NAME, ubuf);
        bool ret = mysql_util::mysql_exec(m_mysql, sql);
        if (ret == false)
        {
            DLOG("select by name: SQL execution failed!");
            return false;
        }
        // 要么有数据，要么没有数据，有数据也只能有一条数据
        MYSQL_RES *res = mysql_store_result(m_mysql);
        if (res == nullptr)
        {
            DLOG("select by name: failed to retrieve result!");
            mysql_free_result(res);
            return false;
        }
        int row_num = mysql_num_rows(res);
        if (row_num == 0)
        {
            DLOG("select by name: wrong username!");
            mysql_free_result(res);
            return false;
        }
        if (row_num > 1)
        {
            DLOG("select by name: duplicate user records found!");
            mysql_free_result(res);
            return false;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        user["id"] = safe_stoi(row[0]);
        user["score"] = safe_stoi(row[1]);
        user["total_count"] = safe_stoi(row[2]);
        user["win_count"] = safe_stoi(row[3]);
        user["username"] = name.c_str();

        mysql_free_result(res);
        return true;

    }
    bool select_by_id(int id, Json::Value &user) // 通过id获取用户信息
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("select by id: database unavailable!");
            return false;
        }
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), USER_BY_ID, id);
        bool ret = mysql_util::mysql_exec(m_mysql, sql);
        if (ret == false)
        {
            DLOG("select by id: SQL execution failed!");
            return false;
        }
        // 要么有数据，要么没有数据，有数据也只能有一条数据
        MYSQL_RES *res = mysql_store_result(m_mysql);
        if (res == nullptr)
        {
            DLOG("select by id: failed to retrieve result!");
            mysql_free_result(res);
            return false;
        }
        int row_num = mysql_num_rows(res);
        if (row_num == 0)
        {
            DLOG("select by id: wrong id!");
            mysql_free_result(res);
            return false;
        }
        if (row_num > 1)
        {
            DLOG("select by id: duplicate user records found!");
            mysql_free_result(res);
            return false;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        user["id"] = id;
        user["username"] = row[0];
        user["score"] = safe_stoi(row[1]);
        user["total_count"] = safe_stoi(row[2]);
        user["win_count"] = safe_stoi(row[3]);

        mysql_free_result(res);
        return true;
    }

    bool win(int id) // 胜利时天梯分数增加30，战斗场次增加1，胜利场次增加1
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("win update: database unavailable!");
            return false;
        }
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), USER_WIN, id);
        bool ret = mysql_util::mysql_exec(m_mysql, sql);
        if (ret == false)
        {
            DLOG("win update: SQL execution failed!");
            return false;
        }

        return true;
    }

    bool lose(int id) // 失败时天梯分数减少30，战斗场次增加1，其他不变
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ensure_connection() == false)
        {
            ELOG("lose update: database unavailable!");
            return false;
        }
        char sql[4096] = { 0 };
        snprintf(sql, sizeof(sql), USER_LOSE, id);
        bool ret = mysql_util::mysql_exec(m_mysql, sql);
        if (ret == false)
        {
            DLOG("lose update: SQL execution failed!");
            return false;
        }

        return true;
    }
};