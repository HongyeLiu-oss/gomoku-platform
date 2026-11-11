#include "logger.hpp"
#include "room.hpp"
#include "server.hpp"
#include "session.hpp"
#include "matcher.hpp"
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <jsoncpp/json/value.h>
#include <string>

// 数据库连接信息放在本地 config.hpp 中(不进仓库)，
// 首次部署请复制 config.example.hpp 为 config.hpp 并填写真实配置
#include "config.hpp"

void mysql_test()
{
    MYSQL *mysql = mysql_util::mysql_create(HOST, USER, PASS, DBNAME, PORT);
    const char *sql = "insert student values (null, '小红', 20, 100, 150, 100);";
    bool ret = mysql_util::mysql_exec(mysql, sql);
    if (ret == false)
        return;
    mysql_util::mysql_destroy(mysql);
}

void json_test()
{
    Json::Value root;
    std::string str;
    root["姓名"] = "小明";
    root["年龄"] = 18;
    root["成绩"].append(140);
    root["成绩"].append(88.5);
    root["成绩"].append(78);
    root["成绩"].append(146);
    
    json_util::serialize(root, str);
    DLOG("%s", str.c_str());

    Json::Value val;
    json_util::unserialize(str, val);
    std::cout << "姓名:" << root["姓名"].asString() << std::endl;
    std::cout << "年龄:" << root["年龄"].asInt() << std::endl;
    int num = root["成绩"].size();
    std::cout << "成绩:";
    for (int i = 0; i < num; i++)
    {
        std::cout << root["成绩"][i].asFloat() <<" ";
    }
}

void string_test()
{
    std::string src = ",...,123,234,,,,,345,,,";
    std::vector<std::string> res;
    string_util::split(src, ",", res);
    for (auto s: res)
    {
        DLOG("%s ", s.c_str());
    }
}

void file_test()
{
    std::string filename = "./makefile";
    std::string body;
    file_util::read(filename, body);

    std::cout << body << std::endl;
}

void db_test()
{
    Json::Value usr;
    user_table table(HOST, USER, PASS, DBNAME, PORT);
    bool ret = table.win(1);
}

void online_test()
{
    online_manager om;
    wsserver_t::connection_ptr conn;
    uint64_t uid = 101;
    om.enter_game_room(uid, conn);
    if (om.is_in_game_room(uid))
        DLOG("IN GAME HALL!");
    else 
        DLOG("NOT IN GAME HALL!");

    om.exit_game_room(uid);
    if (om.is_in_game_room(uid))
        DLOG("IN GAME HALL!");
    else 
        DLOG("NOT IN GAME HALL!");
}

int main()
{
    // 打开日志文件并绑定到 stdout，daemon 后再关闭 stdio 相关描述符
    int fd = open("./gobang.log", O_CREAT | O_APPEND | O_WRONLY, 0644);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);

    // stdout 重定向到普通文件后默认是「全缓冲」,日志会滞留在缓冲区直到写满或进程退出,
    // 无法实时落盘;改为行缓冲,保证每条以 \n 结尾的日志立即写入 gobang.log,便于实时排查
    setvbuf(stdout, nullptr, _IOLBF, 0);

    daemon(1, 1);   // 不切目录 + 不重定向 stdio(自己已处理)

    GobangServer server(HOST, USER, PASS, DBNAME, PORT);
    server.start(8082);

    return 0;
}