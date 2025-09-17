#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <random>
#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace fs = std::filesystem;

struct YCSBRecord {
    std::string key;
    std::map<std::string, std::string> fields;
    std::string operation;
};

class YCSBWorkloadGenerator {
public:
    YCSBWorkloadGenerator(int num_devices = 10, int keys_per_device = 100000, 
                         double zipf_param = 1.2, int cycles = 4, 
                         const std::string& output_dir = "./data")
        : num_devices(num_devices), keys_per_device(keys_per_device), 
          zipf_param(zipf_param), cycles(cycles), output_dir(output_dir) {
        
        fs::create_directories(output_dir);
        
        // 初始化随机数生成器
        rng.seed(std::random_device{}());
        
        // 初始化Zipf分布
        initialize_zipf_distribution();
    }
    
    void initialize_zipf_distribution() {
        // 为每个设备创建Zipf分布
        for (int device_id = 1; device_id <= num_devices; ++device_id) {
            std::vector<double> probabilities(keys_per_device);
            double sum = 0.0;
            
            for (int i = 0; i < keys_per_device; ++i) {
                probabilities[i] = 1.0 / std::pow(i + 1, zipf_param);
                sum += probabilities[i];
            }
            
            for (int i = 0; i < keys_per_device; ++i) {
                probabilities[i] /= sum;
            }
            
            device_distributions[device_id] = std::discrete_distribution<int>(
                probabilities.begin(), probabilities.end());
        }
    }
    
    // 流式生成并保存工作负载，按照设备周期顺序
    void generate_and_save_workload(int requests_per_device = 100000, 
                                   const std::string& filename = "ycsb_workload.txt",
                                   double insert_proportion = 0.0) {
        std::string filepath = output_dir + "/" + filename;
        std::ofstream file(filepath);
        
        if (!file.is_open()) {
            std::cerr << "无法打开文件: " << filepath << std::endl;
            return;
        }
        
        int total_operations = requests_per_device * num_devices * cycles;
        int progress_interval = total_operations / 100; // 每1%显示进度
        
        std::cout << "生成 " << total_operations << " 个操作 (" 
                  << total_operations * insert_proportion << " 插入, " 
                  << total_operations * (1 - insert_proportion) << " 读取)" << std::endl;
        
        // 预生成操作类型序列
        std::vector<std::string> operation_types;
        operation_types.reserve(total_operations);
        
        for (int i = 0; i < total_operations; ++i) {
            if (static_cast<double>(rand()) / RAND_MAX < insert_proportion) {
                operation_types.push_back("INSERT");
            } else {
                operation_types.push_back("READ");
            }
        }
        
        // 流式生成和写入记录，按照设备周期顺序
        int operation_index = 0;
        
        for (int cycle = 0; cycle < cycles; ++cycle) {
            for (int device_id = 1; device_id <= num_devices; ++device_id) {
                for (int i = 0; i < requests_per_device; ++i) {
                    if (operation_index >= total_operations) break;
                    
                    int key_idx = device_distributions[device_id](rng);
                    int key_value = (device_id - 1) * keys_per_device + key_idx + 1;
                    std::string key = "user" + std::to_string(key_value);
                    
                    // 写入操作（YCSB格式）
                    file << operation_types[operation_index] << " usertable " << key;
                    
                    // 如果是INSERT操作，添加字段
                    if (operation_types[operation_index] == "INSERT") {
                        file << " field0=value" << std::to_string(key_value)
                             << " field1=device" << std::to_string(device_id)
                             << " field2=data" << std::to_string(i % 100)
                             << " device_id=" << device_id;
                    } else {
                        // 对于READ操作，也可以添加设备ID作为字段
                        file << " device_id=" << device_id;
                    }
                    
                    file << "\n";
                    
                    operation_index++;
                    
                    // 显示进度
                    if (operation_index % progress_interval == 0) {
                        std::cout << "进度: " << (operation_index * 100 / total_operations) 
                                  << "% (" << operation_index << "/" << total_operations << ")\n";
                    }
                }
            }
        }
        
        file.close();
        std::cout << "YCSB工作负载已保存到: " << filepath << std::endl;
        
        // 创建YCSB配置文件
        create_ycsb_config(filename + ".properties", total_operations);
    }
    
    // 创建YCSB配置文件
    void create_ycsb_config(const std::string& filename, int total_operations) {
        std::string filepath = output_dir + "/" + filename;
        std::ofstream file(filepath);
        
        file << "workload=com.yahoo.ycsb.workloads.CoreWorkload\n";
        file << "recordcount=" << num_devices * keys_per_device << "\n";
        file << "operationcount=" << total_operations << "\n";
        file << "readproportion=1.0\n";
        file << "insertproportion=0.0\n";
        file << "updateproportion=0.0\n";
        file << "requestdistribution=zipfian\n";
        file << "fieldcount=4\n";  // 包括device_id字段
        file << "fieldlength=20\n";
        file << "table=usertable\n";
        file << "measurementtype=histogram\n";
        
        file.close();
        std::cout << "YCSB配置文件已保存到: " << filepath << std::endl;
    }
    
private:
    int num_devices;
    int keys_per_device;
    double zipf_param;
    int cycles;
    std::string output_dir;
    
    std::map<int, std::discrete_distribution<int>> device_distributions;
    std::mt19937 rng;
};

int main() {
    try {
        YCSBWorkloadGenerator generator(
            10,      // num_devices
            100000,  // keys_per_device
            1.2,     // zipf_param
            4,       // cycles
            "./data" // output_dir
        );
        
        // 生成混合工作负载（10% INSERT, 90% READ）
        generator.generate_and_save_workload(100000, "ycsb_workload.txt", 0.0);
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}