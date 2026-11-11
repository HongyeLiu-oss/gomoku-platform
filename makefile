gobang: gobang.cc logger.hpp util.hpp online.hpp db.hpp room.hpp session.hpp server.hpp matcher.hpp
	ccache g++ -o $@ $< -std=c++17 -lboost_system -lpthread -lmysqlclient -ljsoncpp
.PHONY:clean
clean:
	rm gobang