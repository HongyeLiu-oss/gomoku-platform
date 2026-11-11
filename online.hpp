#pragma once
#include "util.hpp"
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>


class online_manager
{
private:
    std::mutex m_mutex;
    // 用于建立游戏大厅的用户的用户id和通信连接的关系
    std::unordered_map<uint64_t, wsserver_t::connection_ptr> m_hall_user;
    // 用于建立游戏房间的用户的用户id和通信连接的关系
    std::unordered_map<uint64_t, wsserver_t::connection_ptr> m_room_user;
public:
    online_manager()
    {}
    // websocket连接建立的时候才会加入游戏大厅&游戏房间的在线用户管理
    void enter_game_hall(uint64_t uid, wsserver_t::connection_ptr &conn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hall_user[uid] = conn;
    }
    void enter_game_room(uint64_t uid, wsserver_t::connection_ptr &conn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_room_user[uid] = conn;
    }
    // websocket连接断开的时候，才会移除游戏大厅&游戏房间的在线用户管理
    void exit_game_hall(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hall_user.erase(uid);
    }
    void exit_game_room(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_room_user.erase(uid);
    }
    // 判断当前指定用户是否在游戏大厅/游戏房间
    bool is_in_game_hall(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hall_user.find(uid);
        if (it == m_hall_user.end())
            return false;
        else
            return true;
    }
    bool is_in_game_room(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_room_user.find(uid);
        if (it == m_room_user.end())
            return false;
        else
            return true;
    }
    // 通过用户id在游戏大厅/游戏房间用户管理中获取对应的通信连接
    wsserver_t::connection_ptr get_conn_from_hall(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hall_user.find(uid);
        if (it == m_hall_user.end())
            return wsserver_t::connection_ptr();
        else
            return it->second;
    }
    wsserver_t::connection_ptr get_conn_from_room(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_room_user.find(uid);
        if (it == m_room_user.end())
            return wsserver_t::connection_ptr();
        else
            return it->second;
    }
    // 判断指定连接是否为该用户当前生效的大厅连接(用于区分"旧连接的关闭事件")
    bool is_hall_conn(uint64_t uid, const wsserver_t::connection_ptr &conn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_hall_user.find(uid);
        if (it == m_hall_user.end())
            return false;
        return it->second.get() == conn.get();
    }
    // 判断指定连接是否为该用户当前生效的房间连接
    bool is_room_conn(uint64_t uid, const wsserver_t::connection_ptr &conn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_room_user.find(uid);
        if (it == m_room_user.end())
            return false;
        return it->second.get() == conn.get();
    }
    // 判断用户是否仍处于活跃状态(存在真实的大厅或房间连接)
    bool is_online(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto hit = m_hall_user.find(uid);
        if (hit != m_hall_user.end() && hit->second.get() != nullptr)
            return true;
        auto rit = m_room_user.find(uid);
        if (rit != m_room_user.end() && rit->second.get() != nullptr)
            return true;
        return false;
    }
    // 统计当前在线人数(大厅与房间中的去重用户数)
    int online_count()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::set<uint64_t> uids;
        for (auto &p : m_hall_user)
            uids.insert(p.first);
        for (auto &p : m_room_user)
            uids.insert(p.first);
        return static_cast<int>(uids.size());
    }
    // 获取所有大厅连接(用于向大厅推送在线人数等广播)
    std::vector<wsserver_t::connection_ptr> get_all_hall_conns()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<wsserver_t::connection_ptr> conns;
        for (auto &p : m_hall_user)
            if (p.second.get() != nullptr)
                conns.push_back(p.second);
        return conns;
    }
    ~online_manager()
    {}
};