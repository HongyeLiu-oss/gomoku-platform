#pragma once
#include "db.hpp"
#include "online.hpp"
#include "room.hpp"
#include "util.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <ios>
#include <jsoncpp/json/value.h>
#include <mutex>
#include <list>
#include <sys/types.h>
#include <thread>
template <class T>
class MatchQueue
{
private:
    std::mutex m_mutex;  // 线程安全
    std::list<T> m_list; // 用链表而不使用queue是因为我们有删除中间数据的需要
    std::condition_variable m_cond; // 这个条件变量主要是为了阻塞消费者，后边使用的时候，队列元素个数小于2则阻塞
    bool m_stop = false; // 是否停止匹配
public:
    MatchQueue() {}
    ~MatchQueue() {}
    // 获取元素个数
    int size()
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        return m_list.size();
    }
    // 判断队列是否为空
    bool empty()
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        return m_list.empty();
    }
    // 停止队列,唤醒所有等待的线程
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock_guard(m_mutex);
            m_stop = true;
        }                    // 先放锁
        m_cond.notify_all(); // 再唤醒,和push()一样的顺序
    }    
    // 检查等待
    void wait_for_match()
    {
        std::unique_lock<std::mutex> ul(m_mutex);
        m_cond.wait(ul, [this]{ return m_list.size() >= 2 || m_stop == true; }); // 原子地:放锁+登记睡眠,不会漏通知
    }
    bool is_stopped()
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        return m_stop;
    }
    // 入队数据，并唤醒线程
    void push(const T &data)
    {
        {   // 进入内层作用域
            std::lock_guard<std::mutex> lock_guard(m_mutex); // 构造:加锁
            m_list.push_back(data);                          // 临界区
        }   // ← 离开作用域,lock_guard 析构:自动解锁
        m_cond.notify_all(); // 走到这里时锁已经释放了
    }
    // 出队数据
    bool pop(T &data)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        if (m_list.empty() == true)
            return false;
        data = m_list.front();
        m_list.pop_front();
        return true;
    }
    // 只移除第一个匹配的元素
    bool erase(const T &data)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        auto it = std::find(m_list.begin(), m_list.end(), data);
        if (it == m_list.end())
            return false;
        m_list.erase(it);
        return true;
    }
    // 判断队列中是否已存在指定元素
    bool exists(const T &data)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        return std::find(m_list.begin(), m_list.end(), data) != m_list.end();
    }
};

class Matcher
{
private:
    // 匹配队列
    MatchQueue<uint64_t> m_average_queue; // score < 2000
    MatchQueue<uint64_t> m_killer_queue;  // 2000 <= score < 3000
    MatchQueue<uint64_t> m_super_queue;   // score >= 3000
    // 对应三个匹配队列的处理线程
    std::thread m_average_thread;
    std::thread m_killer_thread;
    std::thread m_super_thread;
    // 需要的句柄
    RoomManager *m_room_manager;
    user_table *m_user_table;
    online_manager *m_online_manager;
    std::mutex m_mutex; // 保护add/del的"查重+入队"操作原子性
private:
    // 线程接口
    void handle_match(MatchQueue<uint64_t> &queue)
    {
        while (true)
        {
            // 1. 人数不足2则阻塞等待,人数够了或要求停止时返回
            queue.wait_for_match();
            if (queue.is_stopped()) 
                break; // 醒来后发现是"停止"而不是"人够了",直接退出
            // 2. 运行到此时说明人数够了，出队2个玩家
            uint64_t uid1, uid2;
            bool ret = queue.pop(uid1);
            if (ret == false) { continue; }
            ret = queue.pop(uid2);
            if (ret == false)
            {
                // 虽然此时uid2获取失败了，但是uid1已经出队并获取成功了，需要先把uid1再次入队
                queue.push(uid1);
                continue;
            }
            // 3. 校验2个玩家是否在线且不在其他房间,不满足则把另一个人重新加入队列
            wsserver_t::connection_ptr conn1 = m_online_manager->get_conn_from_hall(uid1);
            // 3.1 自愈:玩家若是"匹配预登记"残留(房间表中没有真实房间连接,
            //     即匹配成功后从未真正进入房间,常见于断线/页面加载失败),
            //     先回滚残留状态,再把双方重新入队继续匹配,而不是把玩家丢弃,
            //     否则该玩家会永远无法再次匹配成功
            if (m_online_manager->is_in_game_room(uid1) &&
                m_online_manager->get_conn_from_room(uid1).get() == nullptr)
            {
                m_room_manager->cancel_room_user(uid1);
                queue.push(uid1);
                queue.push(uid2);
                continue;
            }
            if (conn1.get() == nullptr || m_online_manager->is_in_game_room(uid1))
            {
                queue.push(uid2);
                continue;
            }
            wsserver_t::connection_ptr conn2 = m_online_manager->get_conn_from_hall(uid2);
            if (m_online_manager->is_in_game_room(uid2) &&
                m_online_manager->get_conn_from_room(uid2).get() == nullptr)
            {
                m_room_manager->cancel_room_user(uid2);
                queue.push(uid1);
                queue.push(uid2);
                continue;
            }
            if (conn2.get() == nullptr || m_online_manager->is_in_game_room(uid2))
            {
                queue.push(uid1);
                continue;
            }
            // 4. 为2个玩家创建房间，并将玩家加入房间中
            room_ptr rp = m_room_manager->create_room(uid1, uid2);
            if (rp.get() == nullptr)
            {
                // 创建房间失败了，把用户重新添加进队列
                queue.push(uid1);
                queue.push(uid2);
                continue;
            }
            // 房间创建成功,把两个玩家登记为"在房间中"(空连接占位),防止被二次匹配
            // 注意:不能用大厅连接占位!否则大厅连接断开后玩家会永久卡在"房间中"状态,
            // 后续匹配被判定为"已在房间中"而丢弃,再也无法匹配;
            // 真正的房间连接由玩家连接 /room 时在 wsopen_game_room 中覆盖登记
            // 注意:不能调用exit_game_hall,房间内的掉线判断依赖大厅记录
            wsserver_t::connection_ptr null_conn;
            m_online_manager->enter_game_room(uid1, null_conn);
            m_online_manager->enter_game_room(uid2, null_conn);
            // 5. 对2个玩家进行响应
            Json::Value resp;
            resp["optype"] = "match_success";
            resp["result"] = true;
            std::string body;
            json_util::serialize(resp, body);
            try
            {
                conn1->send(body);
                conn2->send(body);
            }
            catch (const std::exception &e)
            {
                ELOG("匹配成功响应发送失败: %s", e.what());
            }
        }
    }
    void thread_average_entry() { return handle_match(m_average_queue); }  
    void thread_killer_entry() { return handle_match(m_killer_queue); }
    void thread_super_entry() { return handle_match(m_super_queue); }
public:
    Matcher(RoomManager *room_manager, user_table *user_table, online_manager *online_manager)
    :m_room_manager(room_manager)
    ,m_user_table(user_table)
    ,m_online_manager(online_manager)
    ,m_average_thread(std::thread(&Matcher::thread_average_entry, this))
    ,m_killer_thread(std::thread(&Matcher::thread_killer_entry, this))
    ,m_super_thread(std::thread(&Matcher::thread_super_entry, this))
    {
        DLOG("游戏匹配模块初始化完毕!");
    }

    ~Matcher() 
    {
        // 1. 先通知三个队列"停止",唤醒可能正在睡觉的线程
        m_average_queue.shutdown();
        m_killer_queue.shutdown();
        m_super_queue.shutdown();
        // 2. 线程被唤醒后会退出while循环,join才能返回
        m_average_thread.join();
        m_killer_thread.join();
        m_super_thread.join();
    }

    // 添加玩家到匹配队列
    bool add(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex); // 保证"查重+入队"的原子性
        // 1. 根据用户id，来获取玩家信息
        Json::Value user;
        bool ret = m_user_table->select_by_id(uid, user);
        if (ret == false)
        {
            DLOG("获取玩家：%ld 信息失败!", uid);
            return false;
        }
        int score = user["score"].asInt();
        // 2. 防止重复入队:已在任一队列中则直接返回
        if (m_average_queue.exists(uid) || m_killer_queue.exists(uid) || m_super_queue.exists(uid))
        {
            DLOG("玩家：%ld 已在匹配队列中!", uid);
            return false;
        }
        // 2.1 已在房间中的玩家不能再次匹配;
        //     若是"匹配预登记"残留(从未真正进入房间),先回滚残留再允许匹配
        if (m_online_manager->is_in_game_room(uid))
        {
            if (m_online_manager->get_conn_from_room(uid).get() == nullptr)
            {
                DLOG("玩家：%ld 存在匹配预登记残留, 回滚后重新匹配!", uid);
                m_room_manager->cancel_room_user(uid);
            }
            else
            {
                DLOG("玩家：%ld 已在房间中!", uid);
                return false;
            }
        }
        // 3. 按天梯分数添加到指定队列中
        if (score < 2000)
            m_average_queue.push(uid);
        else if (score >= 2000 && score < 3000)
            m_killer_queue.push(uid);
        else
            m_super_queue.push(uid);
        return true;
    }

    // 从匹配队列里面移除玩家
    bool del(uint64_t uid)
    {
        // 不依赖"当前分数"定位队列(入队后分数可能已变化),直接从三个队列中尝试移除
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        bool removed = m_average_queue.erase(uid);
        removed |= m_killer_queue.erase(uid);
        removed |= m_super_queue.erase(uid);
        if (removed == false)
            DLOG("玩家：%ld 不在任何匹配队列中!", uid);
        return removed;
    }
};