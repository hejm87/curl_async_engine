all: main.o curl_worker.o
	g++ -o main main.o curl_worker.o -lcurl -lpthread

main.o:
	g++ -std=c++11 -o main.o -c main.cpp

curl_worker.o:
	g++ -std=c++11 -o curl_worker.o -c curl_worker.cpp

clean:
	rm -f main *.o
