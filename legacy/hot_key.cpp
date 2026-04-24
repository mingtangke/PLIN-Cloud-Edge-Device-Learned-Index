#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
int main(){
    std::ifstream in("data/workload_log.csv");
    if(!in){
        std::cerr << "Failed to open input.txt" << std::endl;
        return 1;
    }
    int count = 0;  
    while(in){
        std::string line;
        std::getline(in,line);
        if(line.empty()) continue;

        count++;
    }
    std::cout << count << std::endl;    
    return 0;
}