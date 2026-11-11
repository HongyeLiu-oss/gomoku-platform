#pragma once
#include "logger.hpp"
#include "mysql/mysql.h"
#include "jsoncpp/json/json.h"
#include <assert.h>
#include <cstddef>
#include <fstream>
#include <jsoncpp/json/reader.h>
#include <jsoncpp/json/value.h>
#include <jsoncpp/json/writer.h>
#include <sstream>
#include <string>
#include <memory>
#include <iostream>
#include <vector>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <websocketpp/roles/server_endpoint.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/config/asio_no_tls.hpp>

using wsserver_t = websocketpp::server<websocketpp::config::asio>;

class mysql_util
{
  public:
    static MYSQL* mysql_create(const std::string& host,
                               const std::string& username,
                               const std::string& password,
                               const std::string& dbname, uint16_t port = 3306)
    {
        // 1. 初始化mysql句柄
        MYSQL* mysql = mysql_init(nullptr);
        if (mysql == nullptr)
        {
            ELOG("mysql_init failed!\n");
            return nullptr;
        }

        // 1.1 设置连接超时:断线重连(db.hpp 中的 ensure_connection)时,
        //     若网络异常/数据库不可达,避免长时间阻塞在 mysql_real_connect 上
        unsigned int connect_timeout = 5;
        mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);

        // 2. 连接服务器
        if (nullptr == (mysql_real_connect(mysql, host.c_str(), username.c_str(), password.c_str(), dbname.c_str(), port, nullptr, 0)))
        {
            ELOG("mysql_real_connnect failed : %s", mysql_error(mysql));
            mysql_close(mysql);
            return nullptr;
        }

        // 3. 设置客户端字符集
        if (mysql_set_character_set(mysql, "utf8mb4") != 0)
        {
            ELOG("set mysql character failed : %s", mysql_error(mysql));
            mysql_close(mysql);
            return nullptr;
        }

        return mysql;
    }

    static bool mysql_exec(MYSQL* mysql, const std::string& sql)
    {
        int ret = mysql_query(mysql, sql.c_str());
        if (ret != 0)
        {
            ELOG("%s", sql.c_str());
            ELOG("mysql_query failed : %s", mysql_error(mysql));
            return false;
        }

        return true;
    }

    static void mysql_destroy(MYSQL* mysql)
    {
        if (mysql != nullptr)
        {
            mysql_close(mysql);
        }
        mysql = nullptr;
        return;
    }
};

class json_util
{
public:
    static bool serialize(const Json::Value &root, std::string& str)
    {
        Json::StreamWriterBuilder swb;
        swb["emitUTF8"] = true;
        std::unique_ptr<Json::StreamWriter> sw(swb.newStreamWriter());
        std::stringstream ss;
        int ret = sw->write(root, &ss);
        if (ret != 0)
        {
            ELOG("json serialize failed!");
            return false;
        }
        str = ss.str();
        return true;
    }

    static bool unserialize(const std::string &str, Json::Value &root)
    {
        Json::CharReaderBuilder crb;
        std::unique_ptr<Json::CharReader> cr(crb.newCharReader());
        std::string err;
        bool ret = cr->parse(str.c_str(), str.c_str()+str.size(), &root, &err);
        if (ret == false)
        {
            ELOG("josn unserialize failed: %s", err.c_str());
            return false;
        }

        return true;
    }
    
};

class string_util
{
public:
    // URL百分号解码:将 %E4%B8%AD 之类的编码还原为原始字符,用于中文文件名等
    // 前端 encodeURIComponent 编码后的 URI 经此还原后才能真正匹配到磁盘上的文件
    static std::string url_decode(const std::string &src)
    {
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string dst;
        dst.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (src[i] == '%' && i + 2 < src.size())
            {
                int hi = hex_val(src[i + 1]);
                int lo = hex_val(src[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    dst.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            dst.push_back(src[i]);
        }

        return dst;
    }

    static int split(const std::string &src, const std::string &sep, std::vector<std::string> &res)
    {
        size_t idx = 0, pos = 0;
        while (idx < src.size())
        {
            pos = src.find(sep, idx);
            if (pos == std::string::npos)
            {
                res.push_back(src.substr(idx));
                break;
            }
            if (pos == idx)
            {
                idx += sep.size();
                continue;
            }
            res.push_back(src.substr(idx, pos-idx));
            idx = pos + sep.size();
        }

        return res.size();
    }
};

class file_util
{
public:
    static bool read(const std::string &filename, std::string &body)
    {
        // 打开文件
        std::ifstream ifs(filename, std::ios::binary);
        if (ifs.is_open() == false)
        {
            ELOG("%s file open failed!", filename.c_str());
            return false;
        }
        // 获取文件大小
        size_t fsize = 0;
        ifs.seekg(0, std::ios::end);
        fsize = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        body.resize(fsize);
        // 读取文件
        ifs.read(&body[0], fsize);
        if (ifs.good() == false)
        {
            ELOG("%s file read failed!", filename.c_str());
            ifs.close();
            return false;
        }
        // 关闭文件
        ifs.close();

        return true;
    }
};