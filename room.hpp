#pragma once 
#include "logger.hpp"
#include "online.hpp"
#include "util.hpp"
#include "db.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <jsoncpp/json/forwards.h>
#include <jsoncpp/json/value.h>
#include <memory>
#include <mutex>
#include <random>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

typedef enum { GAME_START, GAME_OVER } room_state;

#define BOARD_ROW 15
#define BOARD_COL 15
#define CHESS_WHITE 1
#define CHESS_BLACK 2
#define TURN_TIME_LIMIT 60 // 每步走棋限时(秒),超时由系统随机落子

class Room : public std::enable_shared_from_this<Room>
{
private:
    uint64_t m_room_id;
    room_state m_state;
    int m_player_count;
    uint64_t m_white_id;
    uint64_t m_black_id;
    uint64_t m_cur_uid; // 当前轮到谁走棋
    user_table *m_tb_user;
    online_manager *m_online_user;
    wsserver_t *m_server;
    std::vector<std::vector<int>> m_board;
    wsserver_t::timer_ptr m_turn_timer; // 走棋倒计时定时器
    std::mt19937_64 m_rng;              // 超时随机落子用
    uint64_t m_turn_seq;                // 走棋计时序号(使过期的定时器回调失效)
    // 对局结束回调:由RoomManager设置,终局时销毁房间并清理uid->房间映射,
    // 否则旧房间残留会导致下一局匹配后请求仍打到GAME_OVER状态的旧房间
    std::function<void(uint64_t)> m_on_game_over;
private:
    bool five(int row, int col, int row_off, int col_off, int color)
    {
        // row和col是下棋位置，row_off和col_off是偏移量，也是方向
        int count = 1;
        int search_row = row + row_off;
        int search_col = col + col_off;
        while (search_row >= 0 && search_row < BOARD_ROW && search_col >= 0 && search_col < BOARD_COL && m_board[search_row][search_col] == color)
        {
            // 同色棋子数量++
            count++;
            search_row += row_off;
            search_col += col_off;
        }
        search_row = row - row_off;
        search_col = col - col_off;
        while (search_row >= 0 && search_row < BOARD_ROW && search_col >= 0 && search_col < BOARD_COL && m_board[search_row][search_col] == color)
        {
            // 同色棋子数量++
            count++;
            search_row -= row_off;
            search_col -= col_off;
        }

        return count >= 5;
    }
    uint64_t check_win(int row, int col, int color) 
    {
        /* 返回胜利玩家的id, 没有胜利者则返回0*/
        /* 从下棋位置的4个方向上分别检测是否出现了5个及以上相同颜色的棋子(横行，纵列，正斜，反斜)*/
        if (five(row, col, 0, 1, color) || five(row, col, 1, 0, color) || five(row, col, -1, 1, color) || five(row, col, -1, -1, color))
        {
            return color == CHESS_WHITE ? m_white_id : m_black_id;
        }
        return 0;
    }
public:
    Room (uint64_t room_id, user_table *tb_user, online_manager *online_user, wsserver_t *server)
    :m_room_id(room_id)
    ,m_state(GAME_START)
    ,m_player_count(0)
    ,m_tb_user(tb_user)
    ,m_online_user(online_user)
    ,m_server(server)
    ,m_white_id(0)
    ,m_black_id(0)
    ,m_cur_uid(0)
    ,m_board(BOARD_ROW, std::vector<int>(BOARD_COL, 0))
    ,m_rng(std::random_device{}())
    ,m_turn_seq(0)
    {
        DLOG("%lu 房间创建成功!", m_room_id);
    }
    ~Room() 
    {
        // 先使定时器回调失效再取消,防止销毁后回调访问已释放对象
        m_turn_seq++;
        cancel_turn_timer();
        DLOG("%lu 房间销毁成功!", m_room_id);
    }
    uint64_t get_id() { return m_room_id; }
    room_state get_state() { return m_state; }
    int get_playercount() { return m_player_count; }
    void add_white_user(uint64_t uid) { m_white_id = uid; m_cur_uid = uid; m_player_count++; } // 白棋先手
    void add_black_user(uint64_t uid) { m_black_id = uid; m_player_count++; }
    uint64_t get_white_user() { return m_white_id; }
    uint64_t get_black_user() { return m_black_id; }
    // 仅供RoomManager注入终局回调
    void set_game_over_callback(std::function<void(uint64_t)> cb) { m_on_game_over = cb; }
    // 对局结束的统一收尾:停止计时,把双方移出"在房间中"状态,
    // 并通过回调销毁房间(清理uid->房间映射),保证下一局能创建全新房间
    void finish_game_over()
    {
        m_state = GAME_OVER;
        m_turn_seq++;
        cancel_turn_timer();
        m_online_user->exit_game_room(m_white_id);
        m_online_user->exit_game_room(m_black_id);
        if (m_on_game_over)
        {
            uint64_t rid = m_room_id;
            m_on_game_over(rid);
        }
    }

    // 启动/取消走棋倒计时(每步限时TURN_TIME_LIMIT秒)
    void start_turn_timer()
    {
        // 先使旧定时器失效(即使取消动作触发回调,序号不匹配也会被忽略)
        m_turn_seq++;
        cancel_turn_timer();
        uint64_t seq = m_turn_seq;
        std::weak_ptr<Room> wp = weak_from_this();
        m_turn_timer = m_server->set_timer(TURN_TIME_LIMIT * 1000, [wp, seq](const std::error_code &) {
            std::shared_ptr<Room> rp = wp.lock();
            if (rp.get() != nullptr)
                rp->on_turn_timeout(seq);
        });
    }
    void cancel_turn_timer()
    {
        if (m_turn_timer.get() != nullptr)
        {
            m_turn_timer->cancel();
            m_turn_timer.reset();
        }
    }
    // 走棋超时:系统随机选一个空位落子,并轮换走棋方
    void on_turn_timeout(uint64_t seq)
    {
        if (seq != m_turn_seq || m_state != GAME_START)
            return; // 已失效的定时器回调或对局已结束
        // 当前走棋方还没有真正进入房间(只有匹配预登记占位):取消对局,不结算战绩
        if (m_online_user->get_conn_from_room(m_cur_uid).get() == nullptr)
        {
            uint64_t other = m_cur_uid == m_white_id ? m_black_id : m_white_id;
            Json::Value json_resp;
            json_resp["optype"] = "put_chess";
            json_resp["result"] = true;
            json_resp["reason"] = "对方未进入房间,对局取消!";
            json_resp["room_id"] = m_room_id;
            json_resp["uid"] = m_cur_uid;
            json_resp["row"] = -1;
            json_resp["col"] = -1;
            json_resp["winner"] = other;
            broadcast(json_resp);
            finish_game_over();
            return;
        }
        std::vector<std::pair<int, int>> empties;
        empties.reserve(BOARD_ROW * BOARD_COL);
        for (int r = 0; r < BOARD_ROW; r++)
            for (int c = 0; c < BOARD_COL; c++)
                if (m_board[r][c] == 0)
                    empties.push_back(std::make_pair(r, c));
        if (empties.empty())
            return;
        std::uniform_int_distribution<size_t> dist(0, empties.size() - 1);
        size_t idx = dist(m_rng);
        Json::Value json_resp = apply_chess(m_cur_uid, empties[idx].first, empties[idx].second,
                                            "超时未落子,系统自动落子!");
        if (json_resp["winner"].asUInt64() != 0)
        {
            uint64_t winner_id = json_resp["winner"].asUInt64();
            uint64_t loser_id = winner_id == m_white_id ? m_black_id : m_white_id;
            m_tb_user->win(winner_id);
            m_tb_user->lose(loser_id);
            json_resp["reason"] = "对方超时,系统自动落子, 五星连珠, 胜利!";
            broadcast(json_resp);
            finish_game_over();
            return;
        }
        start_turn_timer(); // 轮到对方,重新计时
        broadcast(json_resp);
    }
    // 在指定位置为指定玩家落子(不含权限校验),处理棋盘状态、轮换走棋方并判断胜负
    Json::Value apply_chess(uint64_t cur_uid, int row, int col, const std::string &reason)
    {
        Json::Value json_resp;
        json_resp["optype"] = "put_chess";
        json_resp["result"] = true;
        json_resp["reason"] = reason;
        json_resp["room_id"] = m_room_id;
        json_resp["uid"] = cur_uid;
        json_resp["row"] = row;
        json_resp["col"] = col;
        int cur_color = cur_uid == m_white_id ? CHESS_WHITE : CHESS_BLACK;
        m_board[row][col] = cur_color;
        // 落子成功后,轮换走棋方
        m_cur_uid = cur_uid == m_white_id ? m_black_id : m_white_id;
        // 判断是否有玩家胜利（从当前走棋位置开始判断是否存在五星连珠）
        uint64_t winner_id = check_win(row, col, cur_color);
        if (winner_id != 0 && reason.empty())
            json_resp["reason"] = "五星连珠, 胜利!";
        json_resp["winner"] = winner_id;
        return json_resp;
    }

    // 处理下棋动作
    Json::Value handle_chess(Json::Value &req)
    {
        Json::Value json_resp = req; // 返回信息

        uint64_t cur_uid = req["uid"].asUInt64();
        int chess_row = req["row"].asInt();
        int chess_col = req["col"].asInt();
        // 1. 校验走棋者是否是房间中的玩家
        if (cur_uid != m_white_id && cur_uid != m_black_id)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "你不是本房间的玩家!";
            return json_resp;
        }
        // 2. 校验对局是否还在进行(防止终局后重复计分)
        if (m_state != GAME_START)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "对局已经结束!";
            return json_resp;
        }
        // 2.5 双方都必须已真正进入房间(存在真实房间连接)才能落子,
        //     防止一方尚未连接时另一方落子,造成双方棋盘不同步
        if (m_online_user->get_conn_from_room(m_white_id).get() == nullptr ||
            m_online_user->get_conn_from_room(m_black_id).get() == nullptr)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "对方尚未进入房间!";
            return json_resp;
        }
        // 3. 判断房间中2个玩家是否都在线(以房间连接为准),任意一个不在线,就是另一方胜利
        if (m_online_user->is_in_game_room(m_white_id) == false)
        {
            json_resp["result"] = true;
            json_resp["reason"] = "对方掉线! 不战而胜!";
            json_resp["winner"] = m_black_id;
            return json_resp;
        }
        if (m_online_user->is_in_game_room(m_black_id) == false)
        {
            json_resp["result"] = true;
            json_resp["reason"] = "对方掉线! 不战而胜!";
            json_resp["winner"] = m_white_id;
            return json_resp;
        }
        // 4. 校验是否轮到该玩家走棋(白棋先手)
        if (cur_uid != m_cur_uid)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "还没轮到你走棋!";
            return json_resp;
        }
        // 5. 获取走棋位置，判断当前走棋是否合理（越界或位置是否已经被占用）
        if (chess_row < 0 || chess_row >= BOARD_ROW || chess_col < 0 || chess_col >= BOARD_COL)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "走棋位置不合法!";
            return json_resp;
        }
        if (m_board[chess_row][chess_col] != 0)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "当前位置已经有了其他棋子!";
            return json_resp;
        }
        // 6. 落子并判断胜负
        json_resp = apply_chess(cur_uid, chess_row, chess_col, "");
        // 7. 未分胜负则重置走棋倒计时(终局由handle_requset统一停止计时)
        if (json_resp["winner"].asUInt64() == 0)
            start_turn_timer();
        return json_resp;
    }

    // 处理聊天动作
    Json::Value handle_chat(Json::Value &req)
    {
        Json::Value json_resp = req;
        // 2. 校验消息长度(1~256字节),并检测消息中是否包含敏感词
        std::string msg = req["message"].asString();
        if (msg.empty() || msg.size() > 256)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "消息长度不合法!";
            return json_resp;
        }
        size_t pos = msg.find("垃圾");
        if (pos != std::string::npos)
        {
            json_resp["result"] = false;
            json_resp["reason"] = "消息中包含了敏感词，不能发送！";
            return json_resp;
        }
        // 3. 广播消息---返回消息
        json_resp["result"] = true;
        return json_resp;
    }
    // 处理玩家退出房间的动作
    void handle_exit(uint64_t uid)
    {
        // 防御:退出者不是本房间玩家,不结算战绩也不扣减人数,仅清理其房间状态
        if (uid != m_white_id && uid != m_black_id)
        {
            m_online_user->exit_game_room(uid);
            return;
        }
        // 玩家退出房间:先将其从在线管理的"房间表"中移除,使其可以重新参与匹配,
        // 同时避免后续广播向其已断开的连接发送消息
        m_online_user->exit_game_room(uid);
        // 对局进行中退出(认输/掉线/返回大厅):对方不战而胜,必须广播终局消息,
        // 否则对方收不到"游戏结束"通知,会一直卡在房间里
        if (m_state == GAME_START)
        {
            uint64_t winner_id = uid == m_white_id ? m_black_id : m_white_id;
            // 对手尚未入座(只有匹配预登记占位,从未真正进入房间)等异常情况下不结算战绩
            bool opp_entered = winner_id != 0 &&
                m_online_user->get_conn_from_room(winner_id).get() != nullptr;
            if (winner_id != 0 && opp_entered)
            {
                m_tb_user->win(winner_id);
                m_tb_user->lose(uid);
            }
            Json::Value json_resp;
            json_resp["optype"] = "put_chess";
            json_resp["result"] = true;
            json_resp["reason"] = "对方掉线，不战而胜！";
            json_resp["room_id"] = m_room_id;
            json_resp["uid"] = uid;
            json_resp["row"] = -1;
            json_resp["col"] = -1;
            json_resp["winner"] = winner_id;
            // 先广播终局消息再收尾,保证对方能收到"游戏结束"通知
            broadcast(json_resp);
            finish_game_over();
        }
        // 房间中玩家数量--
        m_player_count--;
        return;
    }
    // 总的请求处理函数，在函数内部，区分请求类型, 根据请求类型调用不同的请求处理函数，得到响应就进行广播
    void handle_requset(Json::Value &req)
    {
        // 1. 校验房间号是否匹配
        Json::Value json_resp;
        uint64_t room_id = req["room_id"].asUInt64();
        if (room_id != m_room_id)
        {
            json_resp["optype"] = req["optype"].asString();
            json_resp["result"] = false;
            json_resp["reason"] = "房间号不匹配！";
            return broadcast(json_resp); // 提前结束，不再分发
        }
        // 2. 根据请求类型调用不同的请求处理函数
        if (req["optype"].asString() == "put_chess")
        {
            json_resp = handle_chess(req);
            // 只有请求真正生效(而不是被拒绝/伪造的winner字段)时才结算战绩,
            // 防止客户端在非法请求中夹带winner字段刷战绩;
            // 同时必须标记对局结束,否则房间停留在GAME_START状态,
            // 胜方随后退出房间时会被误判为"对局中掉线",对手收到错误的"不战而胜"
            if (json_resp["result"].isBool() && json_resp["result"].asBool() &&
                json_resp["winner"].asUInt64() != 0)
            {
                uint64_t winner_id = json_resp["winner"].asUInt64();
                uint64_t loser_id = winner_id == m_white_id ? m_black_id : m_white_id;
                m_tb_user->win(winner_id);
                m_tb_user->lose(loser_id);
                m_state = GAME_OVER;
            }
        }
        else if (req["optype"].asString() == "chat")
        {
            json_resp = handle_chat(req);
        }
        else 
        {
            json_resp["optype"] = req["optype"].asString();
            json_resp["result"] = false;
            json_resp["reason"] = "未知请求类型";
        }
        broadcast(json_resp);
        // 对局已分出胜负:广播完终局消息后统一收尾
        // (停止计时+双方退出"在房间中"+销毁房间清理映射,保证下一局是全新房间)
        if (m_state == GAME_OVER)
            finish_game_over();
        return;
    }
    // 将特定的信息广播给房间中的所有玩家
    void broadcast(Json::Value &rsp)
    {
        // 1. 对要响应的信息进行序列化，将Json::Value中的数据序列化为json格式字符串
        std::string body;
        json_util::serialize(rsp, body);
        // 2. 获取房间中所有用户的通信连接
        // 3. 发送响应信息(连接可能正在关闭,发送失败时仅记录日志,避免异常扩散)
        wsserver_t::connection_ptr wconn = m_online_user->get_conn_from_room(m_white_id);
        if (wconn.get() != nullptr)
        {
            try { wconn->send(body); }
            catch (const std::exception &e) { ELOG("广播消息给白棋玩家失败: %s", e.what()); }
        }

        wsserver_t::connection_ptr bconn = m_online_user->get_conn_from_room(m_black_id);
        if (bconn.get() != nullptr)
        {
            try { bconn->send(body); }
            catch (const std::exception &e) { ELOG("广播消息给黑棋玩家失败: %s", e.what()); }
        }

        return;
    }
};

using room_ptr = std::shared_ptr<Room>;

class RoomManager
{
private:
    uint64_t m_room_id_count; // 房间id分配计数器
    user_table *m_tb_user;
    online_manager *m_online_user;
    wsserver_t *m_server;
    std::mutex m_mutex;
    std::unordered_map<uint64_t, room_ptr> m_rooms_by_id; // 房间id和房间信息的映射关系
    std::unordered_map<uint64_t, uint64_t> m_rooms_by_user; // 用户id和房间id的映射关系
public:
    // 初始化房间id计数器
    RoomManager(user_table *tb_user, online_manager *online_user, wsserver_t *server)
    :m_room_id_count(1)
    ,m_tb_user(tb_user)
    ,m_online_user(online_user)
    ,m_server(server)
    {
        DLOG("房间模块初始化完毕!");
    }
    ~RoomManager()
    {
        DLOG("房间模块销毁完毕!");
    }
    // 创建房间
    room_ptr create_room(uint64_t uid1, uint64_t uid2)
    {
        // 两个用户都在游戏大厅中进行匹配，匹配成功后才创建房间
        // 1. 校验2个用户是否还在游戏大厅中，只有都在才需要创建房间
        if (m_online_user->is_in_game_hall(uid1) == false)
        {
            DLOG("用户: %lu 不在大厅中，创建房间失败！", uid1);
            return room_ptr();
        }
        if (m_online_user->is_in_game_hall(uid2) == false)
        {
            DLOG("用户: %lu 不在大厅中，创建房间失败！", uid2);
            return room_ptr();
        }
        // 1.1 防御:清理两个玩家可能残留的旧房间映射(上一局结束后未清理干净的残留),
        //     否则旧映射会覆盖新房间映射,导致请求打到已结束的旧房间
        {
            std::lock_guard<std::mutex> lock_guard(m_mutex);
            m_rooms_by_user.erase(uid1);
            m_rooms_by_user.erase(uid2);
        }
        // 2. 创建房间，将用户信息添加到房间中
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        auto rp = std::make_shared<Room>(m_room_id_count, m_tb_user, m_online_user, m_server);        
        rp->add_white_user(uid1);
        rp->add_black_user(uid2);
        // 注入终局回调:对局结束后立即销毁房间并清理映射
        rp->set_game_over_callback([this](uint64_t rid) { remove_room(rid); });
        // 3. 将房间信息管理起来(用[]覆盖写入,防止同uid残留旧映射导致insert失效)
        m_rooms_by_id[m_room_id_count] = rp;
        m_rooms_by_user[uid1] = m_room_id_count;
        m_rooms_by_user[uid2] = m_room_id_count;
        m_room_id_count++;
        // 4. 返回房间信息
        return rp;
    }
    // 通过房间id查找房间
    room_ptr get_room_by_rid(uint64_t rid)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        auto iter = m_rooms_by_id.find(rid);
        if (iter == m_rooms_by_id.end())
            return room_ptr();
        return iter->second;
    }
    // 通过用户id查找房间
    room_ptr get_room_by_uid(uint64_t uid)
    {
        std::lock_guard<std::mutex> lock_guard(m_mutex);
        // 1. 通过用户id获取房间id
        auto uiter = m_rooms_by_user.find(uid);
        if (uiter == m_rooms_by_user.end())
            return room_ptr();
        uint64_t rid = uiter->second;
        // 2. 通过房间id获取房间信息
        auto riter = m_rooms_by_id.find(rid);
        if (riter == m_rooms_by_id.end())
            return room_ptr();
        return riter->second;
    }
    // 销毁房间
    void remove_room(uint64_t rid)
    {
        // 1. 通过房间id获取房间信息
        room_ptr rp = get_room_by_rid(rid);
        if (rp.get() == nullptr)
            return;
        // 2. 通过房间信息获取房间内所有用户id
        uint64_t uid1 = rp->get_white_user();
        uint64_t uid2 = rp->get_black_user();
        // 3. 兜底:无论何种流程走到销毁房间,都确保两名玩家已退出"在房间中"状态
        //    注意:玩家可能已经离开本房间并进入新房间(用户id→房间id的映射被覆盖),
        //    此时其"房间中"登记属于新房间,不能误删,必须校验映射仍指向本房间
        if (user_in_room(uid1, rid))
            m_online_user->exit_game_room(uid1);
        if (user_in_room(uid2, rid))
            m_online_user->exit_game_room(uid2);
        // 4. 移除房间管理中的用户信息(仅当映射仍指向本房间时)
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it1 = m_rooms_by_user.find(uid1);
        if (it1 != m_rooms_by_user.end() && it1->second == rid)
            m_rooms_by_user.erase(it1);
        auto it2 = m_rooms_by_user.find(uid2);
        if (it2 != m_rooms_by_user.end() && it2->second == rid)
            m_rooms_by_user.erase(it2);
        // 5. 移除房间管理信息
        m_rooms_by_id.erase(rid);
    }
    // 判断指定用户当前是否仍属于指定房间
    bool user_in_room(uint64_t uid, uint64_t rid)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_rooms_by_user.find(uid);
        if (it == m_rooms_by_user.end())
            return false;
        return it->second == rid;
    }
    // 移除房间内的指定用户，若房间中没有用户了，则销毁房间，用户连接断开时被调用
    void remove_room_user(uint64_t uid)
    {
        room_ptr rp = get_room_by_uid(uid);
        if (rp.get() == nullptr)
            return;
        rp->handle_exit(uid);
        // 对局进行中退出时,handle_exit内部已通过终局回调销毁房间;
        // 对局已结束后的正常退出(房间仍残留)则在这里兜底销毁
        if (get_room_by_rid(rp->get_id()).get() != nullptr && rp->get_playercount() == 0)
            remove_room(rp->get_id());
        return;
    }
    // 回滚玩家的"匹配预登记":玩家匹配成功后从未真正进入房间(断线/页面加载失败),
    // 其房间表中登记的只是占位连接,此时需要撤销登记并销毁房间,否则该玩家
    // 会被永久判定为"已在房间中",后续永远无法匹配
    // - 双方都未进入房间:无对局发生,直接销毁房间,不广播、不结算战绩
    // - 对手已进入房间:  通知对手"对方未进入房间,对局取消"后销毁房间,不结算战绩
    void cancel_room_user(uint64_t uid)
    {
        room_ptr rp = get_room_by_uid(uid);
        if (rp.get() == nullptr)
            return;
        uint64_t white = rp->get_white_user();
        uint64_t black = rp->get_black_user();
        uint64_t other = (white == uid) ? black : white;
        // 对手是否已真正进入房间(存在真实房间连接,而非占位)
        bool other_entered = m_online_user->get_conn_from_room(other).get() != nullptr;
        if (other_entered)
        {
            // 通知对手对局取消,让其界面能正常收尾并回到大厅
            Json::Value json_resp;
            json_resp["optype"] = "put_chess";
            json_resp["result"] = true;
            json_resp["reason"] = "对方未进入房间,对局取消!";
            json_resp["room_id"] = rp->get_id();
            json_resp["uid"] = uid;
            json_resp["row"] = -1;
            json_resp["col"] = -1;
            json_resp["winner"] = other;
            rp->broadcast(json_resp);
        }
        // 无论哪种情况:标记终局并统一收尾(双方退出"在房间中"+销毁房间清理映射)
        rp->finish_game_over();
        return;
    }
};

