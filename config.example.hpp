#pragma once
// 数据库连接配置示例。
// 部署时复制本文件为 config.hpp，并填写真实的 MySQL 连接信息：
//   cp config.example.hpp config.hpp
// config.hpp 已被 .gitignore 忽略，不会提交到仓库。

#define HOST "127.0.0.1"          // MySQL 地址
#define USER "root"               // MySQL 用户名
#define PASS "your_password"      // MySQL 密码
#define DBNAME "gobang"           // 数据库名
#define PORT 3306                 // MySQL 端口
