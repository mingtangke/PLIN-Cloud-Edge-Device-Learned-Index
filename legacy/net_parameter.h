#pragma once
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>
#include <random>
#include <cassert>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <sstream>
#define TEST_THREAD 40
#define MAX_MEM_BUFFER_SIZE 3*8192
#define PORT_DEFAULT 9999
size_t DATA_NUM = 1e7;
double UPDATE_CACHE =  0.3 ;