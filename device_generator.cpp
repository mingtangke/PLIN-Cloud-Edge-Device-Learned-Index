#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <random>
#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem; 

struct CSVRecord {
    double timestamp;
    int device_id;
    int key;
    std::string operation;
};

class CSVWriter {
public:
    CSVWriter(const std::string& filename) : file(filename) {
        file << "timestamp,device_id,key,operation\n";
    }
    
    void writeRecord(const CSVRecord& record) {
        file << record.timestamp << ","
             << record.device_id << ","
             << record.key << ","
             << record.operation << "\n";
    }
    
    void close() {
        file.close();
    }
    
private:
    std::ofstream file;
};


class CSVParser {
public:
    CSVParser(const std::string& filename) {
        std::ifstream file(filename);
        std::string line;
        
        std::getline(file, line);
        
        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string token;
            CSVRecord record;
            
            // 解析时间戳
            std::getline(ss, token, ',');
            record.timestamp = std::stod(token);
            
            // 解析设备ID
            std::getline(ss, token, ',');
            record.device_id = std::stoi(token);
            
            // 解析键
            std::getline(ss, token, ',');
            record.key = std::stoi(token);
            
            // 解析操作
            std::getline(ss, token, ',');
            record.operation = token;
            
            records.push_back(record);
        }
    }
    
    const std::vector<CSVRecord>& getRecords() const {
        return records;
    }
    
private:
    std::vector<CSVRecord> records;
};

// 工作负载生成器类
class WorkloadGenerator {
public:
    WorkloadGenerator(int num_devices = 10, int total_keys = 100000, double zipf_param = 1.2,
                     double cycle_duration = 30, int total_cycles = 10, 
                     const std::string& output_dir = "./workload_logs")
        : num_devices(num_devices), total_keys(total_keys), zipf_param(zipf_param),
          cycle_duration(cycle_duration), total_cycles(total_cycles), output_dir(output_dir) {
        
        // 计算每个设备的键数量
        num_keys_per_device = total_keys / num_devices;
        total_time = cycle_duration * num_devices * total_cycles;
        
        // 创建输出目录
        fs::create_directories(output_dir);
        
        // 为每个设备分配键范围
        for (int device_id = 1; device_id <= num_devices; ++device_id) {
            int start_key = (device_id - 1) * num_keys_per_device + 1;
            int end_key = device_id * num_keys_per_device;
            device_key_ranges[device_id] = std::make_pair(start_key, end_key);
        }
        
        // 为每个设备生成Zipf分布的访问概率
        for (int device_id = 1; device_id <= num_devices; ++device_id) {
            std::vector<double> probabilities(num_keys_per_device);
            double sum = 0.0;
            
            // 生成Zipf分布的概率
            for (int i = 0; i < num_keys_per_device; ++i) {
                probabilities[i] = 1.0 / std::pow(i + 1, zipf_param);
                sum += probabilities[i];
            }
            
            // 归一化
            for (int i = 0; i < num_keys_per_device; ++i) {
                probabilities[i] /= sum;
            }
            
            device_access_probs[device_id] = probabilities;
        }
        
        // 初始化随机数生成器
        rng.seed(std::random_device{}());
    }

    std::vector<CSVRecord> debug_generate_workload(int requests_per_device = 500000) {
        std::vector<CSVRecord> logs;
        
        // 计算总请求数
        int total_requests = requests_per_device * num_devices * total_cycles;
        logs.reserve(total_requests);
        int device_id = 0;
        
        // 生成每个周期的请求
        for (int cycle = 0; cycle < 10000000; ++cycle) {
            if(cycle %1000000 == 0 ) device_id ++;
            logs.push_back({0.000237792, device_id, cycle, "find"});
        }
        
        
        return logs;
    }
    

    
    std::vector<CSVRecord> generate_workload(int requests_per_device = 500000) {
        std::vector<CSVRecord> logs;
        
        // 计算总请求数
        int total_requests = requests_per_device * num_devices * total_cycles;
        logs.reserve(total_requests);
        
        // 生成每个周期的请求
        for (int cycle = 0; cycle < total_cycles; ++cycle) {
            // 周期开始和结束时间
            double cycle_start = cycle * cycle_duration * num_devices;
            double cycle_end = (cycle + 1) * cycle_duration * num_devices;
            
            // 为每个设备生成请求
            for (int device_id = 1; device_id <= num_devices; ++device_id) {
                // 设备在周期内的开始和结束时间
                double device_start = cycle_start + (device_id - 1) * cycle_duration;
                double device_end = device_start + cycle_duration;
                
                // 获取该设备的键范围
                auto [start_key, end_key] = device_key_ranges[device_id];
                
                // 创建离散分布用于选择键
                std::discrete_distribution<int> dist(
                    device_access_probs[device_id].begin(), 
                    device_access_probs[device_id].end()
                );
                
                // 生成当前设备的请求
                for (int i = 0; i < requests_per_device; ++i) {
                    // 生成时间戳 (在设备时间段内均匀分布)
                    std::uniform_real_distribution<double> time_dist(0, cycle_duration);
                    double timestamp = device_start + time_dist(rng);
                    
                    // 选择键 (根据Zipf分布)
                    int key_idx = dist(rng);
                    int key = start_key + key_idx;  // 计算实际的键值
                    
                    // 记录日志
                    logs.push_back({timestamp, device_id, key, "find"});
                }
            }
        }
        
        // 按时间戳排序
        std::sort(logs.begin(), logs.end(), [](const CSVRecord& a, const CSVRecord& b) {
            return a.timestamp < b.timestamp;
        });
        
        return logs;
    }
    
    std::string save_workload(const std::vector<CSVRecord>& logs, const std::string& filename = "workload_log.csv") {
        std::string filepath = output_dir + "/" + filename;
        CSVWriter writer(filepath);
        
        for (const auto& record : logs) {
            writer.writeRecord(record);
        }
        for (const auto& record : logs) {
            writer.writeRecord(record);
        }
        
        
        writer.close();
        std::cout << "工作负载已保存到: " << filepath << std::endl;
        return filepath;
    }
    
private:
    int num_devices;
    int total_keys;
    int num_keys_per_device;
    double zipf_param;
    double cycle_duration;
    int total_cycles;
    std::string output_dir;
    double total_time;
    
    std::map<int, std::pair<int, int>> device_key_ranges;  // 设备ID -> (起始键, 结束键)
    std::map<int, std::vector<double>> device_access_probs;  // 设备ID -> 概率分布
    
    std::mt19937 rng;  // 随机数生成器
};



// 使用示例
int main() {
    // 初始化生成器
    WorkloadGenerator generator(
        10,        // num_devices
        10000000,   // total_keys one device:100000 device :hot key = 200000
        1.2,       // zipf_param   3000000*30
        30,        // cycle_duration
        4,         // total_cycles
        "./data"  // output_dir
    );
    
    // 300000 * 20 *2 = 40 * 300000 = 12000000   precache
    auto workload = generator.generate_workload(1000000); 
    
    // 保存工作负载
    std::string log_file = generator.save_workload(workload, "workload_log.csv");
    
    
    // 统计设备访问模式
    std::map<int, int> device_counts;
    for (const auto& record : workload) {
        device_counts[record.device_id]++;
    }
    
    std::cout << "设备访问模式: ";
    for (const auto& [device_id, count] : device_counts) {
        std::cout << "设备" << device_id << ": " << count << "次 ";
    }
    std::cout << std::endl;
    
    return 0;
}