// #pragma once
// #include <fstream>
// #include <string>
// #include <vector>
// #include <thread>
// #include <mutex>
// #include <atomic>
// #include <queue>
// #include <cstring>
// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <arpa/inet.h>
// #include <unistd.h>
// #include "parameters.h"
// #include <unordered_map>
// #include <map>
// #include "utils.h"

// class DatabaseLogger {

// public:
//     DatabaseLogger(const std::string& log_file, const std::string& python_host, int python_port);
//     ~DatabaseLogger();
    
//     void start();
//     void try_start();
//     void stop();
//     void log_query(CSVRecord &log_record);

//     bool prehot_cache = false;
//     bool plin_server_block = false; //currently block during prediction
    
//     std::unordered_map<_key_t,_payload_t> hot_map_;
//     // std::map<_key_t,_payload_t>hot_map_;
//     std::unordered_map<_key_t,_payload_t> log_map_;
//     _key_t *keys;

// private:
    
//     // std::thread logging_thread_;
//     std::thread comm_thread_;
//     std::atomic<bool> running_{false};

//     bool retrain = false;
//     int start_index = 0;
//     int end_index = 0;
//     int transform_count = 0;
//     bool transfer_complete = false;

    
    
    
//     std::mutex log_mutex_;
//     std::queue<CSVRecord> log_queue_;
//     std::mutex hot_map_mutex_;
    
//     // size_t HOT_CACHE = 3000000;
//     size_t HOT_CACHE = 12*1000000 + 50000;  //具体计算过程在device_generator.cpp中 24*1000000
//     // size_t HOT_CACHE = 20000000; //for debug
//     size_t MAX_BUFFER_SIZE = 1000000;
//     size_t MAX_QUEUE_BUFFER_SIZE = 50000;
//     size_t HOT_KEY_NUM = 50000;
//     std::ofstream log_file_;

//     int sockfd_{-1};
//     std::string python_host_;
//     int python_port_;

//     void communication_thread();
// };


// DatabaseLogger::DatabaseLogger(const std::string& log_file, const std::string& python_host, int python_port)
//     : python_host_(python_host), python_port_(python_port) {
//         log_file_.open(log_file, std::ios::app);
//         if (!log_file_.is_open()) {
//         throw std::runtime_error("Failed to open log file");
//         }
//         log_file_<< "timestamp,device_id,key,operation\n";
// }

// DatabaseLogger::~DatabaseLogger() {
//     stop();
//     if (log_file_.is_open()) {
//         log_file_.close();
//     }
//     if (sockfd_ != -1) {
//         close(sockfd_);
//     }
// }


// void DatabaseLogger::log_query( CSVRecord &log_record){
//     std::lock_guard<std::mutex> lock(log_mutex_);
//     _key_t key = log_record.target_key;
//     _payload_t payload = log_record.payload;
//     log_queue_.push(log_record);
//     log_map_[key] = payload;

//     if(log_queue_.size() > MAX_QUEUE_BUFFER_SIZE){    //a big bug for me
//         while( !log_queue_.empty()){
//             // log_file_ << log_queue_.front().timestamp << ","
//             //  << log_queue_.front().device_id << ","
//             //  << std::fixed <<log_queue_.front().target_key << ","
//             //  << log_queue_.front().operation << "\n";

//             log_queue_.pop();
//         }
//         // log_file_.flush();
//         end_index = end_index + MAX_QUEUE_BUFFER_SIZE - 1;
//         // std::cout<<"end_index: "<<end_index<<std::endl;
//     }
// }


// void DatabaseLogger::start() {
//     running_ = true;
    
//     // Setup socket connection to lstm_server
//     sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
//     if (sockfd_ < 0) {
//         throw std::runtime_error("Socket creation failed");
//     }
    
//     struct sockaddr_in serv_addr;
//     serv_addr.sin_family = AF_INET;
//     serv_addr.sin_port = htons(python_port_);
    
//     if (inet_pton(AF_INET, python_host_.c_str(), &serv_addr.sin_addr) <= 0) {
//         throw std::runtime_error("Invalid address/Address not supported");
//     }
    
//     if (connect(sockfd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
//         throw std::runtime_error("Connection to Python failed");
//     }
//     comm_thread_ = std::thread(&DatabaseLogger::communication_thread, this);
// }

// void DatabaseLogger::stop() {
//     running_ = false;
//     if (comm_thread_.joinable()) {
//         comm_thread_.join();
//     }
// }



// void DatabaseLogger::communication_thread() {

//     std::vector<char> buffer(MAX_BUFFER_SIZE, 0);
//     std::ofstream hotkey_file("//home//ming//桌面//PLIN-N //PLIN-N//data//hot_key.csv", std::ios::app);
//     hotkey_file << 'hot_key_count' << transform_count << "\n";
//     transform_count++;
//     std::cout << "Communication thread started" << std::endl;

//     struct timeval tv;
//     tv.tv_sec = 0;
//     tv.tv_usec = 100000; // 100毫秒

//     fd_set readfds;
//     std::string hot_message = "";

//     while (running_) {
//         if (start_index == 0 && end_index >= HOT_CACHE || retrain) {
//             retrain = false;
//             plin_server_block = true;
//             std::string message = "INDEX:" + std::to_string(start_index) + ":" + std::to_string(end_index);
//             std::cout << "Send to python: " << message << std::endl;
//             ssize_t sent_bytes = send(sockfd_, message.c_str(), message.length(), 0);
//             if (sent_bytes == -1) {
//                 perror("send failed");
//             } else {
//                 start_index = end_index + 1;
//             }
//         }

//         FD_ZERO(&readfds);
//         FD_SET(sockfd_, &readfds);
        
//         int activity = select(sockfd_ + 1, &readfds, NULL, NULL, &tv);
        
//         if (activity < 0) {
//             perror("select error");
//             break;
//         } else if (activity == 0) {
//             continue;
//         } else {
//             std::cout << "C++ received python data" <<std::endl;
//             int valread = read(sockfd_, buffer.data(), MAX_BUFFER_SIZE - 1);
//             if (valread > 0) {
//                 buffer[valread] = '\0';
//                 std::string message(buffer.data());
//                 if (!transfer_complete) {
//                     hot_message += message;  
//                 }
//                 if (message.find("END") != std::string::npos) {
//                     transfer_complete = true;
//                 }
//             } else if (valread == 0) {
//                 std::cout << "Python connection closed" << std::endl;
//                 break;
//             } else {
//                 perror("read error");
//                 break;
//             }
//         }

//         if(hot_message.find("END") != std::string::npos && hot_message.find("HOT_KEYS:") == 0 ){
//             std::lock_guard<std::mutex> lock(hot_map_mutex_);
//             hot_map_.clear();
//             hot_map_.reserve(HOT_KEY_NUM * 3); //提高性能
//             std::string keys_str = hot_message.substr(9, hot_message.length() - 12);
//             size_t pos = 0;

//             while ((pos = keys_str.find(",")) != std::string::npos) {
//                 _key_t key =  keys[std::stoi(keys_str.substr(0, pos))];
//                 _payload_t payload = log_map_[key]; 
//                 keys_str.erase(0, pos + 1);
//                 hot_map_[key] = payload;
//                 // hotkey_file << std::fixed << key << " "<<payload<<"\n";
//             }

//             _key_t key = keys[std::stoi(keys_str)];
//             _payload_t payload = log_map_[key];
//              hot_map_[key] = payload;

//             // hotkey_file << std::fixed << key <<" "<<payload<<"\n";
//             // hotkey_file.close();

//             plin_server_block = false ; //main block recovery
//             prehot_cache = true;
//             transfer_complete = false;

//             hot_message = "";
//         }
//     }
// }
#pragma once
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "parameters.h"
#include <unordered_map>
#include <map>
#include <algorithm>
#include "utils.h"
#include <sstream>
#include <future>
#include <set>

class DatabaseLogger {

public:
    DatabaseLogger(const std::string& log_file, const std::string& python_host, int python_port);
    ~DatabaseLogger();
    
    void start();
    void try_start();
    void stop();
    void log_query(CSVRecord &log_record);

    bool prehot_cache = false;
    bool plin_server_block = false; //currently block during prediction

    size_t cache_hit = 0;
    size_t cache_operate = 0;
    double cache_hit_rate = 0;
    
    std::unordered_map<_key_t,_payload_t> hot_map_;
    std::unordered_map<_key_t,_payload_t> log_map_;
    _key_t *keys;

private:
    
    std::thread comm_thread_;
    std::atomic<bool> running_{false};

    bool fine_retrain = false;  //model prediction ，fine turning
    bool all_retrain = false;   //retarin all the model according to the relateed workload and log_record
    bool init_train = false;    //first train the model

    size_t start_index = 0;
    size_t end_index = 0;
    size_t last_trian_index = 0;
    // size_t transform_count = 0;
    bool transfer_complete = false;

    std::set<int> predicted_device;
    // 设备热键频率统计
    std::unordered_map<int, std::unordered_map<_key_t, _payload_t>> device_key_freq_;
    std::mutex freq_mutex_;
    
    std::mutex log_mutex_;
    std::queue<CSVRecord> log_queue_;
    std::mutex hot_map_mutex_;
    
    size_t HOT_CACHE = 12*1000000 + 50000;
    size_t MAX_BUFFER_SIZE = 1000000;
    size_t MAX_QUEUE_BUFFER_SIZE = 50000;
    size_t HOT_KEY_NUM = 50000;
    size_t CACHE_RETRAIN_NUM = 10000;
    double CACHE_RETRAIN_RATE = 0.85;
    std::ofstream log_file_;
    std::string log_file_path_;

    int sockfd_{-1};
    std::string python_host_;
    int python_port_;


    // 异步计算热键的future
    std::future<void> hot_keys_future_;
    bool hot_keys_calculated_ = false;

    void communication_thread();
    void compute_hot_keys_from_log(int start_idx, int end_idx);
    std::vector<_key_t> get_hot_keys_for_device(int device_id, size_t max_keys);
    bool judge_retrain_all(int real_id);
};


DatabaseLogger::DatabaseLogger(const std::string& log_file, const std::string& python_host, int python_port)
    : python_host_(python_host), python_port_(python_port), log_file_path_(log_file) {
        log_file_.open(log_file, std::ios::app);
        if (!log_file_.is_open()) {
        throw std::runtime_error("Failed to open log file");
        }
        log_file_<< "timestamp,device_id,key,operation\n";
}

DatabaseLogger::~DatabaseLogger() {
    stop();
    if (log_file_.is_open()) {
        log_file_.close();
    }
    if (sockfd_ != -1) {
        close(sockfd_);
    }
}

void DatabaseLogger::compute_hot_keys_from_log(int start_idx, int end_idx) {
    std::lock_guard<std::mutex> lock(freq_mutex_);
    device_key_freq_.clear();
    
    std::ifstream log_file(log_file_path_);
    if (!log_file.is_open()) {
        std::cerr << "Failed to open log file for reading: " << log_file_path_ << std::endl;
        return;
    }
    
    std::string line;
    int current_line = 0;

    std::getline(log_file, line);
    while (std::getline(log_file, line) && current_line <= end_idx) {
        if (current_line >= start_idx) {
            std::istringstream iss(line);
            std::string timestamp_str, device_id_str, key_str, operation_str;
            
            if (std::getline(iss, timestamp_str, ',') &&
                std::getline(iss, device_id_str, ',') &&
                std::getline(iss, key_str, ',') &&
                std::getline(iss, operation_str, ',')) {
                
                try {
                    int device_id = std::stoi(device_id_str);
                    _key_t key = std::stod(key_str);
                    
                    // 更新设备键频率
                    device_key_freq_[device_id][key]++;
                } catch (const std::exception& e) {
                    std::cerr << "Error parsing log line: " << line << " - " << e.what() << std::endl;
                }
            }
        }
        current_line++;
    }
    
    log_file.close();
    hot_keys_calculated_ = true;
    std::cout << "Computed hot keys from log lines " << start_idx << " to " << end_idx << std::endl;
}

std::vector<_key_t> DatabaseLogger::get_hot_keys_for_device(int device_id, size_t max_keys) {
    std::lock_guard<std::mutex> lock(freq_mutex_);
    
    if (device_key_freq_.find(device_id) == device_key_freq_.end()) {
        return {};
    }
    
    auto& key_freq = device_key_freq_[device_id];
    std::vector<std::pair<_key_t, int>> key_freq_vec(key_freq.begin(), key_freq.end());
    std::sort(key_freq_vec.begin(), key_freq_vec.end(),
        [](const std::pair<_key_t, int>& a, const std::pair<_key_t, int>& b) {
            return a.second > b.second;
        });
    std::vector<_key_t> hot_keys;
    for (size_t i = 0; i < std::min(max_keys, key_freq_vec.size()); i++) {
        hot_keys.push_back(key_freq_vec[i].first);
    }
    
    return hot_keys;
}

void DatabaseLogger::log_query(CSVRecord &log_record){
    std::lock_guard<std::mutex> lock(log_mutex_);
    _key_t key = log_record.target_key;
    _payload_t payload = log_record.payload;
    log_queue_.push(log_record);
    log_map_[key] = payload;

    if(log_queue_.size() > MAX_QUEUE_BUFFER_SIZE){
        while(!log_queue_.empty()){
            log_file_ << log_queue_.front().timestamp << ","
             << log_queue_.front().device_id << ","
             << std::fixed << log_queue_.front().target_key << ","
             << log_queue_.front().operation << "\n";

            log_queue_.pop();
        }
        log_file_.flush();
        end_index = end_index + MAX_QUEUE_BUFFER_SIZE - 1;
    }
}

void DatabaseLogger::start() {
    running_ = true;
    
    // Setup socket connection to lstm_server
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        throw std::runtime_error("Socket creation failed");
    }
    
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(python_port_);
    
    if (inet_pton(AF_INET, python_host_.c_str(), &serv_addr.sin_addr) <= 0) {
        throw std::runtime_error("Invalid address/Address not supported");
    }
    
    if (connect(sockfd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        throw std::runtime_error("Connection to Python failed");
    }
    comm_thread_ = std::thread(&DatabaseLogger::communication_thread, this);
}

void DatabaseLogger::stop() {
    running_ = false;
    if (comm_thread_.joinable()) {
        comm_thread_.join();
    }
}


bool DatabaseLogger::judge_retrain_all(int real_id){
    auto item = predicted_device.find(real_id);
    if(item == predicted_device.end()){
        all_retrain = true;
    }
}

void DatabaseLogger::communication_thread() {
    std::vector<char> buffer(MAX_BUFFER_SIZE, 0);
    // std::ofstream hotkey_file("//home//ming//桌面//PLIN-N //PLIN-N//data//hot_key.csv", std::ios::app);
    // hotkey_file << 'hot_key_count' << transform_count << "\n";
    // transform_count++;
    std::cout << "Communication thread started" << std::endl;

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100毫秒

    fd_set readfds;
    std::string message_str = "";

    while (running_) {
        if(cache_operate > CACHE_RETRAIN_NUM && (1.0*cache_hit/cache_operate < CACHE_RETRAIN_RATE)){
            cache_operate = 0;
            cache_hit_rate = 0;
            fine_retrain = true;
        }

        if((start_index == 0 && end_index >= HOT_CACHE)){
            init_train = true;
        }


        if (init_train || fine_retrain || all_retrain) {
            plin_server_block = true;
            std::string message = "";
            if(fine_retrain){
                message = "ADJUST:" + std::to_string(start_index) + ":" + std::to_string(end_index);
                fine_retrain = false;
            }else if(all_retrain){
                std::cout<<"The workload model has changed!"<<std::endl;
                message = "INDEX:" + std::to_string(last_trian_index) + ":" + std::to_string(end_index);
                start_index = end_index;
                all_retrain = false;
            }
            else{
                assert(start_index == 0);
                message = "INDEX:" + std::to_string(start_index) + ":" + std::to_string(end_index);
                last_trian_index = end_index;
                init_train = false;
            }
            start_index = end_index;

            std::cout << "Send to python: " << message << std::endl;
            ssize_t sent_bytes = send(sockfd_, message.c_str(), message.length(), 0);
            if (sent_bytes == -1) {
                perror("send failed");
            } else {
                hot_keys_calculated_ = false;
                // hot_keys_future_ = std::async(std::launch::async, 
                //     &DatabaseLogger::compute_hot_keys_from_log, this, start_index, end_index);
                compute_hot_keys_from_log(start_index,end_index);
                start_index = end_index + 1;
            }
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd_, &readfds);
        
        int activity = select(sockfd_ + 1, &readfds, NULL, NULL, &tv);
        
        if (activity < 0) {
            perror("select error");
            break;
        } else if (activity == 0) {
            continue;
        } else {
            std::cout << "C++ received python data" << std::endl;
            int valread = read(sockfd_, buffer.data(), MAX_BUFFER_SIZE - 1);
            if (valread > 0) {
                buffer[valread] = '\0';
                std::string message(buffer.data());
                if (!transfer_complete) {
                    message_str += message;  
                }
                if (message.find("END") != std::string::npos) {
                    transfer_complete = true;
                }
            } else if (valread == 0) {
                std::cout << "Python connection closed" << std::endl;
                break;
            } else {
                perror("read error");
                break;
            }
        }

        if(message_str.find("END") != std::string::npos && message_str.find("DEVICES:") == 0 && hot_keys_calculated_) {
            // if (hot_keys_future_.valid() && hot_keys_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            //     std::cout << "Waiting for hot keys calculation to complete..." << std::endl;
            //     hot_keys_future_.wait();
            // }
            
            std::lock_guard<std::mutex> lock(hot_map_mutex_);
            hot_map_.clear();
            hot_map_.reserve(HOT_KEY_NUM * 2 * 2); // better O(n)/O(1)
            
            std::string devices_str = message_str.substr(8, message_str.length() - 11); // remove "DEVICES:" && "END"
            size_t comma_pos = devices_str.find(',');
            
            if (comma_pos != std::string::npos) {
                int device1 = std::stoi(devices_str.substr(0, comma_pos));
                int device2 = std::stoi(devices_str.substr(comma_pos + 1));
                
                std::cout << "Received predicted devices: " << device1 << ", " << device2 << std::endl;
                predicted_device.emplace(device1);
                predicted_device.emplace(device2);

                // device 2 key
                std::vector<_key_t> hot_keys1 = get_hot_keys_for_device(device1, HOT_KEY_NUM);
                for (_key_t key : hot_keys1) {
                    auto it = log_map_.find(key);
                    if (it != log_map_.end()) {
                        hot_map_[key] = it->second;
                        // hotkey_file << std::fixed << key << " " << it->second << "\n";
                    }
                }
                
                // device 1 key
                std::vector<_key_t> hot_keys2 = get_hot_keys_for_device(device2, HOT_KEY_NUM);
                for (_key_t key : hot_keys2) {
                    auto it = log_map_.find(key);
                    if (it != log_map_.end()) {
                        hot_map_[key] = it->second;
                        // hotkey_file << std::fixed << key << " " << it->second << "\n";
                    }
                }
                
                // hotkey_file.close();
                
                plin_server_block = false; // main block recovery
                prehot_cache = true;
                transfer_complete = false;
                message_str = "";
                
                std::cout << "Hot map updated with " << hot_map_.size() << " keys" << std::endl;
            } else {
                std::cerr << "Invalid devices message format: " << message_str << std::endl;
                message_str = "";
            }
        }
    }
}