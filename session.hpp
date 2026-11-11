#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>
#include "util.hpp"

typedef enum { UNLOGIN, LOGIN } ss_statu;

class Session
{
private:
    uint64_t m_ssid; // 标识符
    uint64_t m_uid; // 用户id
    ss_statu m_statu; // 用户状态: 已登录，未登录
    wsserver_t::timer_ptr m_tp; // session关联的定时器
    uint64_t m_timer_seq; // 定时器序号:每次挂新定时器时+1,使旧定时器的过期回调失效
public:
    Session(uint64_t ssid)
    :m_ssid(ssid)
    ,m_statu(UNLOGIN)
    ,m_uid(1)
    ,m_timer_seq(0)
    { DLOG("SESSION %p 被创建！", this); }
    ~Session() { DLOG("SESSION %p 被销毁！", this); }
    uint64_t ssid() { return m_ssid; }
    void set_status(ss_statu statu) { m_statu = statu; }
    void set_user(uint64_t uid) { m_uid = uid; }
    uint64_t get_user() { return m_uid; }
    bool is_login() { return (m_statu == LOGIN); }
    void set_timer(const wsserver_t::timer_ptr &tp) { m_tp = tp; }
    wsserver_t::timer_ptr& get_timer() { return m_tp; }
    uint64_t next_timer_seq() { return ++m_timer_seq; }
    uint64_t timer_seq() { return m_timer_seq; }
};

#define SESSION_TIMEOUT 30000
#define SESSION_FOREVER -1
using session_ptr = std::shared_ptr<Session>;
class SessionManager
{
private:
    std::mutex m_mutex;
    std::mt19937_64 m_rng; // 随机session id生成器(防止会话id被猜中劫持)
    std::unordered_map<uint64_t, session_ptr> m_sessions_by_id; // session id 和 session信息的映射关系
    wsserver_t *m_server;
public:
    SessionManager(wsserver_t *server)
    :m_server(server)
    ,m_rng(std::random_device{}())
    {
        DLOG("session管理器初始化完毕!");
    }
    ~SessionManager()
    {
        DLOG("session管理器销毁!");
    }
    session_ptr create_session(uint64_t uid, ss_statu statu)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        // 随机生成不与现有会话冲突的非0会话id
        uint64_t ssid = 0;
        do
        {
            ssid = m_rng();
        } while (ssid == 0 || m_sessions_by_id.count(ssid) != 0);
        session_ptr ssp = std::make_shared<Session>(ssid);
        ssp->set_user(uid);
        ssp->set_status(statu);
        m_sessions_by_id.insert(std::make_pair(ssid, ssp));
        return ssp;
    }
    session_ptr get_session_by_ssid(uint64_t ssid)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        auto it = m_sessions_by_id.find(ssid);
        if (it == m_sessions_by_id.end())
            return session_ptr();
        return it->second;
    }
    void remove_session(uint64_t ssid)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        m_sessions_by_id.erase(ssid);
    }
    // 设置会话的过期时间
    // sec == SESSION_FOREVER(-1): 永久存在(取消一切过期定时器)
    // sec > 0:                    sec毫秒后删除会话
    void set_session_expire_time(uint64_t ssid, int sec)
    {
        // 登录之后，创建session，session需要在指定时间无通信后删除
        // 但是进入游戏大厅，或者游戏房间，这个session就应该永久存在
        // 等到退出游戏大厅，或者游戏房间，这个session应该被重新设置为临时，在长时间无通信后被删除
        session_ptr ssp = get_session_by_ssid(ssid);
        if (ssp.get() == nullptr)
            return;
        // 1. 先取消旧定时器并置空
        //    注意:被cancel的定时器回调仍会以"操作取消"错误码被触发一次,
        //    下面的回调内部通过错误码+序号双重校验,保证不会误删会话
        wsserver_t::timer_ptr old_tp = ssp->get_timer();
        if (old_tp.get() != nullptr)
            old_tp->cancel();
        ssp->set_timer(wsserver_t::timer_ptr());
        // 2. 要求永久存在:不再挂任何定时器
        if (sec == SESSION_FOREVER)
            return;
        // 3. 要求定时删除:挂新定时器,并记录当前序号
        uint64_t seq = ssp->next_timer_seq();
        wsserver_t::timer_ptr tmp_tp = m_server->set_timer(sec, [this, ssid, seq](const std::error_code &ec) {
            if (ec)
                return; // 定时器被取消时以错误码触发,直接忽略
            this->remove_session_with_seq(ssid, seq);
        });
        ssp->set_timer(tmp_tp);
    }
    // 定时器到期回调:仅当序号与当前一致时才真正删除会话(旧回调一律忽略)
    void remove_session_with_seq(uint64_t ssid, uint64_t seq)
    {
        session_ptr ssp = get_session_by_ssid(ssid);
        if (ssp.get() == nullptr)
            return;
        if (seq != ssp->timer_seq())
            return; // 已失效的旧定时器回调
        remove_session(ssid);
    }
};