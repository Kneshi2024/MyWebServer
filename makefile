target=server
threadpool:=$(wildcard ThreadPool/*.hpp)
main:=$(wildcard *.cpp)
http_conn:=$(wildcard http/*.h http/*.cpp)
lock:=$(wildcard lock/*.h)
log:=$(wildcard log/*.h log/*.cpp)
cgimysql:=$(wildcard CGImysql/*.h CGImysql/*.cpp)
$(target):$(main) $(http_conn) $(lock) $(log) $(cgimysql)
	g++ -std=c++11 -o $@ $^ -lpthread -lmysqlclient

clean:
	rm  -r server
