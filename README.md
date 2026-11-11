## 📌 简介
一个基于 C++ / WebSocket 实现的在线五子棋对战平台。支持用户注册登录、随机匹配对战、实时落子同步、积分排行，并内置背景音乐播放，前端为原生 HTML/CSS/JS，后端为高性能 C++ 服务器。

---

## ✨ 功能特性
* **用户体系**：注册 / 登录 / 退出，密码采用 SHA2-256 加盐存储，登录失败限流防爆破。
* **在线对战**：随机匹配对手，实时 WebSocket 同步落子与对局状态。
* **积分排行**：胜负加减分 (score / total_count / win_count) 实时统计。
* **大厅推送**：在线人数实时刷新推送。
* **背景音乐**：内置音乐列表，支持中文文件名，支持播放/暂停/上下曲与流式 Range 请求。
* **安全机制**：静态资源防跨域保护、会话 Cookie 校验、统一的 JSON 响应封装。

---

## 🛠️ 技术栈
* **语言 / 标准**：C++17
* **核心框架**：WebSocket++ , Boost.Asio
* **数据库**：MySQL
* **数据序列化**：Jsoncpp
* **前端技术**：HTML / CSS / JS (jQuery)

---

## 📦 依赖
在编译和运行前，请确保系统已安装以下依赖：
* `g++` (支持 C++17)
* Boost 库 (`libboost-system-dev`)
* MySQL 客户端库 (`libmysqlclient-dev`) / MySQL 服务
* Jsoncpp 开发库 (`libjsoncpp-dev`)

> 💡 安装命令(以 Ubuntu / Debian 为例)：
> ```bash
> sudo apt-get update
> sudo apt-get install -y g++ make libboost-system-dev libssl-dev \
>     libmysqlclient-dev libjsoncpp-dev libwebsocketpp-dev mysql-server
> ```

---

## 🚀 部署步骤

**1. 初始化数据库 (创建库与数据表)**
```bash
mysql -u root -p < db.sql
```

**2. 配置数据库连接**
复制配置示例并填写你的 MySQL 账号密码：
```bash
cp config.example.hpp config.hpp
```
```cpp
#define HOST "127.0.0.1"   // MySQL 地址
#define USER "root"        // MySQL 用户名
#define PASS "你的密码"     // MySQL 密码
#define DBNAME "gobang"    // 数据库名(默认即可)
#define PORT 3306          // MySQL 端口
```
> 💡 提示：`config.hpp` 已被 `.gitignore` 忽略，不会提交到仓库；修改后需要重新 `make` 才会生效。

**3. 编译**
```bash
make
```

**4. 启动服务**
```bash
./gobang
```
服务器默认监听 **8082** 端口(绑定所有网卡)，启动成功后日志无报错即正常运行。

**5. 访问**
浏览器打开：
```
http://<服务器IP>:8082/login.html
```
> ⚠️ 若无法访问，请检查防火墙是否放行 8082 端口：
> ```bash
> # Ubuntu
> sudo ufw allow 8082
> # CentOS
> sudo firewall-cmd --add-port=8082/tcp --permanent && sudo firewall-cmd --reload
> ```

---

## 📁 目录结构
```
├── gobang.cc          程序入口(服务器启动)
├── config.example.hpp 数据库连接配置示例(复制为 config.hpp 使用)
├── server.hpp         HTTP/WebSocket 服务核心(静态资源、路由、会话、音乐列表)
├── matcher.hpp        匹配系统
├── room.hpp           对局房间管理
├── online.hpp         在线用户管理
├── session.hpp        会话管理
├── db.hpp             数据库操作
├── util.hpp           通用工具(JSON、文件读写、URL 解码)
├── logger.hpp         日志
├── db.sql             数据库初始化脚本
├── makefile           构建脚本
└── wwwroot/           前端静态资源(登录/大厅/对局页、音乐目录)
```

---

## 📝 使用说明
1. 打开页面后先**注册**账号，再登录。
2. 登录后进入**游戏大厅**，点击「开始匹配」，系统自动为你匹配对手。
3. 对局中双方轮流落子，先连成五子者获胜。
4. 点击顶部音乐栏可播放背景音乐(把音乐文件放入 `wwwroot/music/` 目录即可，**中文文件名也能正常播放**)。

---

## ❓ 常见问题

**Q: 音乐播放不了？**
- 确认音频文件在 `wwwroot/music/` 目录下，支持 `mp3 / m4a / aac / ogg / wav / flac / webm` 等格式；
- 中文文件名已支持(服务端会做 URL 解码)，若仍异常请刷新页面重新加载列表。

**Q: 登录提示失败？**
- 检查 `gobang.cc` 中的数据库账号密码是否配置正确；
- 确认已执行过 `db.sql` 完成建库建表。

**Q: 想修改服务端口？**
- 修改 `gobang.cc` 中 `server.start(8082)` 的端口号后重新 `make`。

---

## 📄 License
本项目仅用于学习交流。
