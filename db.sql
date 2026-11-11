create database if not exists gobang;
use gobang;
create table if not exists user
(
    id int primary key auto_increment,
    username varchar(32) unique key not null,
    password varchar(64) not null,
    score int,
    total_count int,
    win_count int
);