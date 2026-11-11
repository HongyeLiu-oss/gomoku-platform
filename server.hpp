#pragma once
#include "db.hpp"
#include "matcher.hpp"
#include "online.hpp"
#include "room.hpp"
#include "session.hpp"
#include "util.hpp"
#include <bits/types/FILE.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <jsoncpp/json/value.h>
#include <set>
#include <string>
#include <unordered_map>
#include <websocketpp/http/constants.hpp>
#include <websocketpp/http/request.hpp>

#define WWWROOT "./wwwroot/"

class GobangServer
{
private:
    std::string m_web_root; // 静态资源根目录
    wsserver_t m_wsserver;
    user_table m_userTable;
    online_manager m_onlineManager;
    RoomManager m_roomManager;
    Matcher m_matcher;
    SessionManager m_sessionManager;
    // 登录防爆破:记录每个用户名的失败次数(简单限流)
    struct LoginFail { int count = 0; time_t last = 0; };
    std::mutex m_login_mutex;
    std::unordered_map<std::string, LoginFail> m_login_fails;
private:
    void file_handler(wsserver_t::connection_ptr &conn)
    {
        // 静态资源请求的处理
        // 1. 获取到uri资源请求的路径，了解客户端请求的页面文件名称
        websocketpp::http::parser::request req = conn->get_request();
        std::string uri = req.get_uri();
        // 去掉查询字符串(如 /login.html?tab=register),避免路径拼接错误
        size_t qpos = uri.find('?');
        if (qpos != std::string::npos)
            uri = uri.substr(0, qpos);
        // 对URI做百分号解码:前端会用encodeURIComponent编码中文等特殊字符,
        // 必须先还原成真实文件名,才能正确找到磁盘上的文件
        uri = string_util::url_decode(uri);
        // 2. 组合出文件的实际路径：相对根目录+uri
        std::string real_path = m_web_root + uri;
        // 3. 如果请求是一个目录，增加一个后缀:login.html
        if (real_path.back() == '/')
            real_path += "login.html";
        // 4. 路径穿越防护:解析真实路径,必须位于静态资源根目录内(防../目录穿越读取任意文件)
        std::string safe_path;
        bool path_ok = false;
        do
        {
            char *resolved = realpath(real_path.c_str(), nullptr);
            if (resolved == nullptr)
                break;
            safe_path.assign(resolved);
            free(resolved);
            char *root = realpath(m_web_root.c_str(), nullptr);
            if (root == nullptr)
                break;
            std::string safe_root(root);
            free(root);
            if (safe_path.size() < safe_root.size() ||
                safe_path.compare(0, safe_root.size(), safe_root) != 0)
                break;
            // 前缀匹配后必须正好是根目录本身,或根目录下的文件(下一个字符是'/')
            if (safe_path.size() > safe_root.size() && safe_path[safe_root.size()] != '/')
                break;
            path_ok = true;
        } while (false);
        if (path_ok == false)
        {
            // 文件不存在或路径非法,返回404
            conn->set_status(websocketpp::http::status_code::not_found);
            conn->replace_header("Content-Type", "text/html; charset=utf-8");
            std::string error_body = "<html><head><meta charset='utf-8'><title>404 Not Found</title></head><body><h1>404 Not Found</h1><p>请求的文件不存在</p></body></html>";
            conn->set_body(error_body);
            return;
        }
        // 5. 读取文件内容
        std::string body;
        bool ret = file_util::read(safe_path, body);
        if (ret == false)
        {
            // 若读取失败，返回404
            conn->set_status(websocketpp::http::status_code::not_found);
            conn->replace_header("Content-Type", "text/html; charset=utf-8");
            std::string error_body = "<html><head><meta charset='utf-8'><title>404 Not Found</title></head><body><h1>404 Not Found</h1><p>请求的文件不存在</p></body></html>";
            conn->set_body(error_body);
            return;
        }
        // 6. 根据文件后缀设置Content-Type(音频/视频/页面等都需要正确的MIME)
        std::string content_type = "application/octet-stream";
        std::string ext;
        size_t dot = safe_path.find_last_of('.');
        if (dot != std::string::npos)
            ext = safe_path.substr(dot);
        if (ext == ".html") content_type = "text/html; charset=utf-8";
        else if (ext == ".css") content_type = "text/css; charset=utf-8";
        else if (ext == ".js") content_type = "application/javascript; charset=utf-8";
        else if (ext == ".json") content_type = "application/json; charset=utf-8";
        else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";
        else if (ext == ".png") content_type = "image/png";
        else if (ext == ".gif") content_type = "image/gif";
        else if (ext == ".svg") content_type = "image/svg+xml";
        else if (ext == ".ico") content_type = "image/x-icon";
        else if (ext == ".mp3") content_type = "audio/mpeg";
        else if (ext == ".mp4") content_type = "video/mp4";
        else if (ext == ".m4a") content_type = "audio/mp4";
        else if (ext == ".ogg") content_type = "audio/ogg";
        else if (ext == ".wav") content_type = "audio/wav";
        else if (ext == ".webm") content_type = "audio/webm";
        else if (ext == ".flac") content_type = "audio/flac";
        // 6.5 静态资源缓存策略:
        //     图片/音频/视频等体积大的媒体文件缓存1天,避免每次进房间都重复下载大文件
        //     (公网带宽有限时, 重复下载会明显拖慢棋盘背景图等资源的加载);
        //     html/css/js 保持不缓存, 保证页面改版后能即时生效
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
            ext == ".svg" || ext == ".ico" || ext == ".webp" ||
            ext == ".mp3" || ext == ".mp4" || ext == ".m4a" ||
            ext == ".ogg" || ext == ".wav" || ext == ".webm" || ext == ".flac")
            conn->replace_header("Cache-Control", "public, max-age=86400");
        // 7. 支持Range请求(浏览器流式播放音频/视频、拖动进度条都需要)
        conn->replace_header("Content-Type", content_type);
        std::string range_header = conn->get_request_header("Range");
        bool range_ok = false;
        long long rstart = 0, rend = -1;
        if (range_header.compare(0, 6, "bytes=") == 0)
        {
            std::string range_val = range_header.substr(6);
            size_t dash = range_val.find('-');
            if (dash != std::string::npos)
            {
                try
                {
                    rstart = std::stoll(range_val.substr(0, dash));
                    if (dash + 1 < range_val.size())
                        rend = std::stoll(range_val.substr(dash + 1));
                }
                catch (...)
                {
                    rstart = -1; // 解析失败,走完整返回
                }
                if (rstart >= 0 && rstart < (long long)body.size())
                {
                    if (rend < 0 || rend >= (long long)body.size())
                        rend = (long long)body.size() - 1;
                    if (rend >= rstart)
                        range_ok = true;
                }
            }
        }
        if (range_ok)
        {
            std::string part = body.substr((size_t)rstart, (size_t)(rend - rstart + 1));
            conn->set_status(websocketpp::http::status_code::partial_content);
            conn->append_header("Content-Range", "bytes " + std::to_string(rstart) + "-" +
                std::to_string(rend) + "/" + std::to_string(body.size()));
            conn->append_header("Accept-Ranges", "bytes");
            conn->set_body(part);
            return;
        }
        // 8. 设置响应正文
        conn->set_status(websocketpp::http::status_code::ok);
        conn->append_header("Accept-Ranges", "bytes");
        conn->set_body(body);
    }

    // 统一的JSON响应发送函数
    void send_json_response(wsserver_t::connection_ptr& conn,
                            bool success,
                            const std::string& reason,
                            int status_code = websocketpp::http::status_code::ok)
    {
        Json::Value resp_json;
        resp_json["result"] = success;
        resp_json["reason"] = reason;

        std::string resp_body;
        json_util::serialize(resp_json, resp_body);

        conn->set_status(static_cast<websocketpp::http::status_code::value>(status_code));
        conn->set_body(resp_body);
        conn->append_header("Content-Type", "application/json");
    }

    // 统一的WebSocket JSON响应发送函数 (websocket直接send,http是set_body)
    void send_ws_json_response(wsserver_t::connection_ptr& conn,
                               const std::string& optype,
                               bool result,
                               const std::string& reason)
    {
        Json::Value resp_json;
        resp_json["optype"] = optype;
        resp_json["reason"] = reason;
        resp_json["result"] = result;

        std::string body;
        json_util::serialize(resp_json, body);
        conn->send(body);
    }

    void reg(wsserver_t::connection_ptr& conn)
    {
        // 用户注册功能请求的处理
        websocketpp::http::parser::request req = conn->get_request();
        // 1. 获取到请求正文
        std::string req_body = conn->get_request_body();
        // 2. 对正文进行json反序列化,得到用户名和密码
        Json::Value login_info;
        bool ret1 = json_util::unserialize(req_body, login_info);
        if (ret1 == false)
        {
            DLOG("反序列化注册信息失败!");
            send_json_response(conn, false, "请求的正文格式错误!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 3. 检查用户名和密码是否为空
        if (login_info["username"].isNull() || login_info["password"].isNull())
        {
            DLOG("用户名或密码不完整!");
            send_json_response(conn, false, "请输入用户名/密码!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 4. 进行数据库的用户新增操作
        bool ret2 = m_userTable.insert(login_info);
        if (ret2 == false)
        {
            DLOG("向数据库插入数据失败!");
            send_json_response(conn, false, "用户名已经被占用!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 注册成功
        send_json_response(conn, true, "注册用户成功!");
    }
    void login(wsserver_t::connection_ptr& conn)
    {
        // 用户登录功能请求的处理
        websocketpp::http::parser::request req = conn->get_request();
        // 1. 获取到请求正文,对正文进行json反序列化,得到用户名和密码
        std::string req_body = conn->get_request_body();
        Json::Value login_info;
        bool ret1 = json_util::unserialize(req_body, login_info);
        if (ret1 == false)
        {
            DLOG("反序列化登录信息失败!");
            send_json_response(conn, false, "请求的正文格式错误!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 2. 校验正文完整性,进行数据库的用户信息验证
        if (login_info["username"].isNull() || login_info["password"].isNull() ||
            login_info["username"].asString().empty() || login_info["password"].asString().empty())
        {
            DLOG("用户名或密码不完整!");
            send_json_response(conn, false, "请输入用户名/密码!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 2.1 登录防爆破:同一用户名1分钟内失败5次则拒绝
        std::string uname = login_info["username"].asString();
        {
            std::lock_guard<std::mutex> lock(m_login_mutex);
            auto it = m_login_fails.find(uname);
            if (it != m_login_fails.end() && it->second.count >= 5 &&
                time(nullptr) - it->second.last < 60)
            {
                send_json_response(conn, false, "尝试次数过多,请1分钟后再试!",
                                   websocketpp::http::status_code::bad_request);
                return;
            }
        }
        bool ret2 = m_userTable.login(login_info);
        if (ret2 == false)
        {   // 登录失败 返回400,并记录失败次数
            DLOG("用户名或密码错误!");
            std::lock_guard<std::mutex> lock(m_login_mutex);
            LoginFail &fail = m_login_fails[uname];
            if (time(nullptr) - fail.last > 60)
                fail.count = 0;
            fail.count++;
            fail.last = time(nullptr);
            send_json_response(conn, false, "用户名或密码错误!",
                               websocketpp::http::status_code::bad_request);
            return;
        }
        // 登录成功,清除该用户名的失败记录
        {
            std::lock_guard<std::mutex> lock(m_login_mutex);
            m_login_fails.erase(uname);
        }
        // 3. 如果创建成功，给客户端创建Session
        uint64_t uid = login_info["id"].asUInt64();
        session_ptr ssp = m_sessionManager.create_session(uid, LOGIN);
        if (ssp.get() == nullptr)
        {
            DLOG("创建会话失败!");
            send_json_response(conn, false, "创建会话失败!",
                               websocketpp::http::status_code::internal_server_error);
            return;
        }
        // 设置session失效时间
        m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_TIMEOUT);
        // 4. 设置响应头部: Set-Cookie,将sessionid通过cookie返回
        //    HttpOnly防XSS窃取Cookie; SameSite=Strict防跨站请求携带Cookie
        std::string cookie_ssid = "SSID=" + std::to_string(ssp->ssid()) + "; Path=/; HttpOnly; SameSite=Strict";
        conn->append_header("Set-Cookie", cookie_ssid);
        return send_json_response(conn, true, "登录成功", websocketpp::http::status_code::ok);
    }
    // 从Cookie中获取SSID 
    bool get_cookie_val(const std::string &cookie_str, const std::string &key, std::string &val)
    {
        // Cookie: SSID=xxx; path=/; 
        // 1. 以"; "作为间隔，对字符串进行分割，得到各个单个的Cookie信息
        std::string sep = "; ";
        std::vector<std::string> cookie_arr;
        string_util::split(cookie_str, sep, cookie_arr);
        for (auto str : cookie_arr)
        {
            // 2. 对单个的cookie字符串，以"="为间隔进行分割，得到key和val
            std::vector<std::string> tmp_arr;
            string_util::split(str, "=", tmp_arr);
            if (tmp_arr.size() != 2)
                continue;
            if (tmp_arr[0] == key)
            {
                val = tmp_arr[1];
                return true;
            }
        }
        return false;
    }

    // 从Cookie中获取用户Session,并区分失败原因
    // 返回值: true表示获取成功(ssp有效); false表示失败(reason记录了错误原因)
    bool get_session_by_cookie(const wsserver_t::connection_ptr &conn,
                               session_ptr &ssp,
                               std::string &reason)
    {
        // 1. 获取请求信息中的Cookie, 从Cookie中获取ssid
        std::string cookie_str = conn->get_request_header("Cookie");
        if (cookie_str.empty())
        {
            // 如果没有Cookie,返回错误;没有Cookie信息,让客户端重新登录
            reason = "没有找到Cookie信息,请重新登录!";
            return false;
        }
        // 从Cookie中提取ssid
        std::string ssid_str;
        bool ret = get_cookie_val(cookie_str, "SSID", ssid_str);
        if (ret == false)
        {
            // Cookie中没有ssid,返回错误;让客户端重新登录
            reason = "没有找到SSID信息,请重新登录!";
            return false;
        }
        // 2. 安全解析ssid:必须为纯数字,防止非法输入导致std::stol抛异常使服务器崩溃
        errno = 0;
        char *end_ptr = nullptr;
        unsigned long long ssid = std::strtoull(ssid_str.c_str(), &end_ptr, 10);
        if (errno != 0 || end_ptr == ssid_str.c_str() || *end_ptr != '\0')
        {
            reason = "登录信息无效,请重新登录!";
            return false;
        }
        // 3. 在session管理中查找对应的会话信息
        ssp = m_sessionManager.get_session_by_ssid(static_cast<uint64_t>(ssid));
        if (ssp.get() == nullptr)
        {
            // 如果没有Session,则认为登录已经过期,需要重新登录
            reason = "登录过期,请重新登录!";
            return false;
        }
        return true;
    }

    void info(wsserver_t::connection_ptr &conn)
    {
        // 用户信息获取功能请求的处理
        // 1. 校验登录状态,获取用户的Session
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return send_json_response(conn, false, reason, websocketpp::http::status_code::bad_request);
        // 2. 从数据库中取出用户信息,进行序列化发送给客户端
        uint64_t uid = ssp->get_user();
        Json::Value user_info;
        bool ret = m_userTable.select_by_id(uid, user_info);
        if (ret == false)
        {
            // 获取用户信息失败，返回错误，找不到用户信息
            return send_json_response(conn, false, "没有找到用户信息!", websocketpp::http::status_code::bad_request);
        }
        std::string body;
        json_util::serialize(user_info, body);
        conn->set_body(body);
        conn->set_status(websocketpp::http::status_code::ok);
        conn->append_header("Content-Type", "application/json");
        // 4. 刷新Session的过期时间
        refresh_session_alive(ssp);
    }
    // 刷新Session的存活状态:
    // 玩家仍在大厅或房间(存在真实长连接)时Session应永久存在,
    // 否则按"长时间无通信"处理,30秒后自动过期删除
    // 注意:房间页加载时会请求/info,如果无条件把Session重置为30秒过期,
    // 对局超过30秒后玩家就会在走棋时被误判为"登录过期"而踢下线
    void refresh_session_alive(const session_ptr &ssp)
    {
        if (m_onlineManager.is_online(ssp->get_user()))
            m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_FOREVER);
        else
            m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_TIMEOUT);
    }
    // 向大厅所有在线用户推送当前在线人数
    void broadcast_online_count()
    {
        Json::Value msg;
        msg["optype"] = "online_count";
        msg["count"] = m_onlineManager.online_count();
        std::string body;
        json_util::serialize(msg, body);
        std::vector<wsserver_t::connection_ptr> conns = m_onlineManager.get_all_hall_conns();
        for (auto &c : conns)
        {
            if (c.get() == nullptr)
                continue;
            try { c->send(body); }
            catch (const std::exception &e) { ELOG("推送在线人数失败: %s", e.what()); }
        }
    }
    void music_list(wsserver_t::connection_ptr &conn)
    {
        // 音乐列表获取:扫描wwwroot/music目录下的音频文件
        Json::Value resp;
        Json::Value arr(Json::arrayValue);
        std::string music_dir = m_web_root + "music";
        try
        {
            std::error_code ec;
            if (std::filesystem::is_directory(music_dir, ec) && !ec)
            {
                std::set<std::string> files;
                for (auto &entry : std::filesystem::directory_iterator(music_dir, ec))
                {
                    if (ec)
                        break;
                    if (!entry.is_regular_file(ec))
                        continue;
                    std::string name = entry.path().filename().string();
                    std::string ext;
                    size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos)
                        ext = name.substr(dot);
                    if (ext == ".mp3" || ext == ".mp4" || ext == ".m4a" || ext == ".aac" ||
                        ext == ".ogg" || ext == ".wav" || ext == ".flac" || ext == ".webm")
                        files.insert(name);
                }
                for (auto &f : files)
                    arr.append(f);
            }
        }
        catch (const std::exception &e)
        {
            ELOG("扫描音乐目录失败: %s", e.what());
        }
        resp["result"] = true;
        resp["files"] = arr;
        std::string body;
        json_util::serialize(resp, body);
        conn->set_status(websocketpp::http::status_code::ok);
        conn->replace_header("Content-Type", "application/json; charset=utf-8");
        conn->set_body(body);
    }
    void logout(wsserver_t::connection_ptr &conn)
    {
        // 用户退出登录功能请求的处理
        // 1. 校验登录状态,获取用户的Session
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return send_json_response(conn, false, reason, websocketpp::http::status_code::bad_request);
        // 2. 清理该用户在大厅/房间中的在线登记(可能残留)
        m_onlineManager.exit_game_hall(ssp->get_user());
        m_onlineManager.exit_game_room(ssp->get_user());
        // 3. 销毁Session
        m_sessionManager.remove_session(ssp->ssid());
        // 4. 清除客户端Cookie
        conn->append_header("Set-Cookie", "SSID=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        return send_json_response(conn, true, "退出登录成功!");
    }

private:
    void http_callback(websocketpp::connection_hdl hdl)
    {
        wsserver_t::connection_ptr conn = m_wsserver.get_con_from_hdl(hdl);
        websocketpp::http::parser::request req = conn->get_request();
        std::string method = req.get_method();
        std::string uri = req.get_uri();
        
        if (method == "POST" && uri == "/reg")
            return reg(conn);
        else if (method == "POST" && uri == "/login")
            return login(conn);
        else if (method == "GET" && uri == "/info")
            return info(conn);
        else if (method == "GET" && uri == "/music_list")
            return music_list(conn);
        else if (method == "POST" && uri == "/logout")
            return logout(conn);
        else
            return file_handler(conn);
    }

    void wsopen_game_hall(wsserver_t::connection_ptr &conn)
    {
        // 游戏大厅的长连接建立成功
        // 1. 登录验证,判断当前用户是否已经登录成功
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return send_ws_json_response(conn, "hall_ready", false, reason);
        // 2. 重复登录处理:同用户的新连接直接顶替旧的残留连接
        //    旧连接往往已经失效(标签页关闭/网络异常但未触发清理),如果直接拒绝新连接,
        //    前端会把 hall_ready=false 当作登录失败踢回登录页,造成"登录后马上退回登录页";
        //    顶替后旧连接的登记被清除,残留状态随之自动修复
        m_onlineManager.exit_game_hall(ssp->get_user());
        // 3. 将当前客户端以及连接加入到游戏大厅
        m_onlineManager.enter_game_hall(ssp->get_user(), conn);
        // 4. 给客户端响应游戏大厅连接建立成功
        send_ws_json_response(conn, "hall_ready", true, "");
        // 5. 记得将session设置为永久存在
        m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_FOREVER);
        // 6. 向大厅所有用户推送最新在线人数
        broadcast_online_count();
    }

    void wsopen_game_room(wsserver_t::connection_ptr &conn)
    {
        // 1. 获取当前客户端的Session连接
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return send_ws_json_response(conn, "room_ready", false, reason);
        // 2. 判断当前用户是否已经创建好了房间(房间管理)
        room_ptr rp = m_roomManager.get_room_by_uid(ssp->get_user());
        if (rp == nullptr)
            return send_ws_json_response(conn, "room_ready", false, "没有找到玩家的房间信息!");
        // 3. 清理可能残留的大厅登记,避免重复登录误判
        //    注意:匹配成功时matcher已经用"空连接占位"将玩家预登记进了游戏房间
        //    (m_room_user),此时is_in_game_room必然为true,属于正常流程,不能拦截;
        //    页面跳转时旧大厅连接可能尚未完成关闭处理,直接清除残留的大厅登记即可,
        //    不要拒绝新连接,否则房间页会因room_ready=false而无法正常初始化
        m_onlineManager.exit_game_hall(ssp->get_user());
        // 4. 将当前用户登记到在线用户管理的游戏房间中(用房间连接覆盖匹配时预登记的大厅连接)
        m_onlineManager.enter_game_room(ssp->get_user(), conn);
        // 4.1 启动走棋倒计时(白棋先手,每步限时1分钟;后进入的玩家会把计时器重置为满时限)
        rp->start_turn_timer();
        // 5. 将Session重新设置为永久存在
        m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_FOREVER);
        // 6. 响应房间连接建立成功,并携带房间信息供前端初始化棋盘
        //    注意:前端收到room_ready会用其中的字段覆盖room_info并初始化棋盘,
        //    这里只能发送这一条room_ready,不能再用send_ws_json_response重复回复
        Json::Value room_info;
        room_info["optype"] = "room_ready";
        room_info["result"] = true;
        room_info["reason"] = "";
        room_info["room_id"] = rp->get_id();
        room_info["uid"] = ssp->get_user();
        room_info["white_id"] = rp->get_white_user();
        room_info["black_id"] = rp->get_black_user();
        // 棋盘规格下发, 前端据此初始化棋盘(避免前后端棋盘尺寸不一致)
        room_info["board_size"] = BOARD_ROW;
        // 附带双方用户名,供前端显示"你为白棋/黑棋"及聊天消息的发送者
        std::string white_name = "未知";
        std::string black_name = "未知";
        Json::Value name_info;
        if (m_userTable.select_by_id(static_cast<int>(rp->get_white_user()), name_info))
            white_name = name_info["username"].asString();
        if (m_userTable.select_by_id(static_cast<int>(rp->get_black_user()), name_info))
            black_name = name_info["username"].asString();
        room_info["white_name"] = white_name;
        room_info["black_name"] = black_name;
        std::string body;
        json_util::serialize(room_info, body);
        conn->send(body);
    }

    void wsopen_callback(websocketpp::connection_hdl hdl)
    {
        // websocket长连接建立成功之后的处理函数
        wsserver_t::connection_ptr conn = m_wsserver.get_con_from_hdl(hdl);
        websocketpp::http::parser::request req = conn->get_request();
        std::string uri = req.get_uri();
        if (uri == "/hall")
        {
            // 建立了游戏大厅的长连接
            return wsopen_game_hall(conn);
        }
        else if (uri == "/room")
        {
            // 建立了游戏房间的长连接
            return wsopen_game_room(conn);
        }
    }

    void wsclose_game_hall(wsserver_t::connection_ptr &conn)
    {
        // 游戏大厅的长连接断开的处理
        // 1. 登录验证,获取当前用户的Session
        //    连接正在关闭,失败时直接返回,不能再向对端发送消息
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return;
        uint64_t uid = ssp->get_user();
        // 2. 仅当关闭的是"当前生效"的大厅连接时才移除登记:
        //    玩家刷新/重进大厅时会建立新连接,旧连接随后才触发关闭事件,
        //    若无条件移除,会把新连接刚建立的大厅登记一并清掉,导致在线状态丢失
        if (m_onlineManager.is_hall_conn(uid, conn))
        {
            m_onlineManager.exit_game_hall(uid);
            // 2.1 向大厅推送最新在线人数
            broadcast_online_count();
        }
        // 3. 玩家若已进入房间(房间连接已建立),大厅断开不影响对局,Session应保持永久;
        //    只有玩家彻底不活跃时才恢复"30秒无通信过期"的临时生命周期
        //    (注意:大厅关闭与房间建立是两条独立的连接,处理顺序不固定,
        //     这里不能无条件设置过期,否则正在对局的玩家会被误判过期踢下线)
        if (m_onlineManager.is_online(uid) == false)
            m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_TIMEOUT);
    }

    void wsclose_game_room(wsserver_t::connection_ptr &conn)
    {
        // 游戏房间的长连接断开的处理
        // 1. 获取会话信息(连接已断开,失败时直接返回,不能再向对端发送消息)
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return;
        uint64_t uid = ssp->get_user();
        // 2. 仅当关闭的是"当前生效"的房间连接时才处理玩家退出:
        //    玩家刷新/重进房间时会建立新连接,旧连接随后才触发关闭事件,
        //    若无条件处理,会把已经用新连接重新入房的玩家误判为掉线并终结对局
        if (m_onlineManager.is_room_conn(uid, conn))
        {
            // remove_room_user内部会:胜负未分则判对方胜利并广播,
            // 将玩家从在线管理的房间表中移除(exit_game_room),
            // 房间中所有用户都退出后销毁房间
            m_roomManager.remove_room_user(uid);
            // 2.1 向大厅推送最新在线人数
            broadcast_online_count();
        }
        // 3. 玩家若已回到大厅(大厅连接已建立),房间断开不影响大厅,Session应保持永久;
        //    只有玩家彻底不活跃时才恢复临时生命周期
        if (m_onlineManager.is_online(uid) == false)
            m_sessionManager.set_session_expire_time(ssp->ssid(), SESSION_TIMEOUT);
    }

    void wsclose_callback(websocketpp::connection_hdl hdl)
    { 
        // websocket长连接断开前的处理
        wsserver_t::connection_ptr conn = m_wsserver.get_con_from_hdl(hdl);
        websocketpp::http::parser::request req = conn->get_request();
        std::string uri = req.get_uri();
        if (uri == "/hall")
        {
            // 游戏大厅的长连接断开前的处理
            return wsclose_game_hall(conn);
        }
        else if (uri == "/room")
        {
            // 建立了游戏房间的长连接
            return wsclose_game_room(conn);
        }
    }

    void wsmsg_game_hall(wsserver_t::connection_ptr &conn, wsserver_t::message_ptr &msg)
    {
        // 1. 身份验证,获取客户端玩家信息
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
            return send_ws_json_response(conn, "hall_ready", false, reason);
        // 2. 获取请求信息
        std::string req_body = msg->get_payload();
        Json::Value req_json;
        bool ret = json_util::unserialize(req_body, req_json);
        if (ret == false)
            return send_ws_json_response(conn, "unknow", false, "请求信息解析失败!");
        // 3. 对于请求信息进行处理
        if (!req_json["optype"].isNull() && req_json["optype"].asString() == "match_start")
        {
            // 开始对战匹配:通过匹配模块,将用户添加到匹配队列中
            m_matcher.add(ssp->get_user());
            return send_ws_json_response(conn, "match_start", true, "");
        }
        else if (!req_json["optype"].isNull() && req_json["optype"].asString() == "match_stop")
        {
            // 停止对战匹配:通过匹配模块,将用户移除匹配队列
            m_matcher.del(ssp->get_user());
            return send_ws_json_response(conn, "match_stop", true, "");
        }
        else
        {
            return send_ws_json_response(conn, "unknow", false, "请求类型未知!");
        }
    }

    void wsmsg_game_room(wsserver_t::connection_ptr &conn, wsserver_t::message_ptr &msg)
    {
        // 1. 获取客户端Session, 识别客户端身份
        session_ptr ssp;
        std::string reason;
        if (get_session_by_cookie(conn, ssp, reason) == false)
        {
            DLOG("房间:没有找到会话信息!");
            return send_ws_json_response(conn, "room_ready", false, reason);
        }
        // 2. 获取客户端房间信息
        room_ptr rp = m_roomManager.get_room_by_uid(ssp->get_user());
        if (rp == nullptr)
        {
            DLOG("房间:没有找到玩家的房间信息!");
            return send_ws_json_response(conn, "unknow", false, "没有找到玩家的房间信息!");
        }
        // 3. 对消息进行反序列化
        Json::Value req_json;
        std::string req_body = msg->get_payload();
        bool ret = json_util::unserialize(req_body, req_json);
        if (ret == false)
        {
            DLOG("房间:反序列化请求失败!");
            return send_ws_json_response(conn, "unknow", false, "请求信息解析失败!");
        }
        // 4. 通过房间模块进行消息请求的处理
        DLOG("房间:收到房间请求,开始处理!");
        return rp->handle_requset(req_json);
    }

    void wsmsg_callback(websocketpp::connection_hdl hdl, wsserver_t::message_ptr msg)
    {
        // websocket长连接通信处理
        wsserver_t::connection_ptr conn = m_wsserver.get_con_from_hdl(hdl);
        websocketpp::http::parser::request req = conn->get_request();
        std::string uri = req.get_uri();
        if (uri == "/hall")
        {
            // 游戏大厅的长连接通信处理
            return wsmsg_game_hall(conn, msg);
        }
        else if (uri == "/room")
        {
            // 建立了游戏房间的长连接
            return wsmsg_game_room(conn, msg);
        }

    }
public:
  // 进行成员初始化，以及服务器回调函数的设置
    GobangServer(const std::string &host,
                const std::string &username,
                const std::string &password,
                const std::string &dbname, 
                uint16_t port = 3306,
                const std::string &wwwroot = WWWROOT)
                :m_web_root(wwwroot)
                ,m_userTable(host, username, password, dbname, port)
                ,m_onlineManager()
                ,m_roomManager(&m_userTable, &m_onlineManager, &m_wsserver)
                ,m_sessionManager(&m_wsserver)
                ,m_matcher(&m_roomManager, &m_userTable, &m_onlineManager)
    {
        // 设置日志等级
        m_wsserver.set_access_channels(websocketpp::log::alevel::none); // 禁止日志
        // 初始化asio调度器
        m_wsserver.init_asio();
        m_wsserver.set_reuse_addr(true);
        // 限制消息与请求体大小,防止超大消息耗尽服务器资源
        m_wsserver.set_max_message_size(1024 * 1024);   // websocket消息最大1MB
        m_wsserver.set_max_http_body_size(64 * 1024);   // http请求体最大64KB
        // 设置回调函数
        m_wsserver.set_http_handler(std::bind(&GobangServer::http_callback, this, std::placeholders::_1));
        m_wsserver.set_open_handler(std::bind(&GobangServer::wsopen_callback, this, std::placeholders::_1));
        m_wsserver.set_close_handler(std::bind(&GobangServer::wsclose_callback, this, std::placeholders::_1));
        m_wsserver.set_message_handler(std::bind(&GobangServer::wsmsg_callback, this, std::placeholders::_1, std::placeholders::_2));
    }
    // 启动服务器
    void start(int port)
    {
        // listen 在端口被占用时会抛出 websocketpp::exception,若不捕获会直接 terminate 崩溃;
        // 这里统一捕获,记录日志后正常返回,避免"老进程没退干净又起一个"时进程直接挂掉
        try
        {
            m_wsserver.listen(port);
            m_wsserver.start_accept();
            m_wsserver.run();
        }
        catch (const std::exception &e)
        {
            ELOG("服务器启动/运行失败: %s", e.what());
        }
    }
};