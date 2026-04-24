#pragma once
#include "inner_node.h"
#include "piecewise_linear_model.h"
#include <iostream>
#include <fstream>
#include <cstdint>
#include "unordered_map"
#include "inner_node.h"

typedef struct t_record{
    _key_t key;
    LeafNode *addr;
    bool valid;
    void serialize(std::ostream &out) const {
        out.write(reinterpret_cast<const char*>(&key), sizeof(key));
        out.write(reinterpret_cast<const char*>(&addr), sizeof(addr));
        out.write(reinterpret_cast<const char*>(&valid), sizeof(valid));
    }
    
    void deserialize(std::istream &in) {
        in.read(reinterpret_cast<char*>(&key), sizeof(key));
        in.read(reinterpret_cast<char*>(&addr), sizeof(addr));
        in.read(reinterpret_cast<char*>(&valid), sizeof(valid));
    }
}t_record;

typedef struct param{
    _key_t first_key;
    double slope;
    double intercept;
    uint64_t block_num;


    void serialize(std::ostream& os) const {
        os.write(reinterpret_cast<const char*>(&first_key), sizeof(first_key)); 
        os.write(reinterpret_cast<const char*>(&slope), sizeof(slope));
        os.write(reinterpret_cast<const char*>(&intercept), sizeof(intercept));
        os.write(reinterpret_cast<const char*>(&block_num), sizeof(block_num));
    }

    void deserialize(std::istream& is) {
        is.read(reinterpret_cast<char*>(&first_key), sizeof(first_key));
        is.read(reinterpret_cast<char*>(&slope), sizeof(slope));
        is.read(reinterpret_cast<char*>(&intercept), sizeof(intercept));
        is.read(reinterpret_cast<char*>(&block_num), sizeof(block_num));
    }
}Param;

class local_model{

private:
    _key_t *keys;
    _payload_t *payloads;
    // size_t number = 1e8;
    uint64_t number = 1e7;
    constexpr static uint64_t InnerSlotsPerBlock = 8UL;

public: 
    std::vector<t_record>local_table;
    // std::unordered_map<LeafNode *,std::vector<_key_t>> train_data;
    _key_t min_key;
    _key_t max_key;
    size_t valid_leaf;
    // double slope,intercept;
    // std::atomic<bool> is_training{false};
    std::vector<std::vector<Param>>parameter;
    
    // int predict_pos(_key_t key){ //lower bound!
    //     _key_t predicted_value = key;

    //     int level = parameter.size();
    //     while(level > 0){
    //         std::vector<Param> level_params = parameter[level-1];
    //         bool found_segment = false;
    //         size_t size = level_params.size();
    //         size_t temp_pos = 0;
    //         size_t k = 0;
    //         // 2 4 6 8 10 12
    //         while(k < size){
    //             if(predicted_value >= level_params[k].first_key) {
    //                 k++;
    //             }else
    //                 break;
    //         }
    //         temp_pos = k;
    //         if(k >= size) temp_pos = size - 1;
    //         // else temp_pos = k - 1;
    //         predicted_value = level_params[temp_pos].slope * (predicted_value - level_params[temp_pos].first_key) + level_params[temp_pos].intercept;
    //         level --;
    //     }
    //     // // predicted_value = predicted_value;
    //     // if(predicted_value < 0) predicted_value = 0;
    //     // if(predicted_value > leaf_num - 1) predicted_value = leaf_num - 1;
    //     return int(predicted_value);
    // }
    int predict_pos(_key_t key){ //lower bound!
        _key_t predicted_value = key;

        int level = parameter.size();
        while(level > 0){
            std::vector<Param> level_params = parameter[level-1];
            bool found_segment = false;
            size_t size = level_params.size();
            size_t temp_pos = 0;
            size_t k = 0;
            // 2 4 6 8 10 12
            while(k < size){
                if(predicted_value >= level_params[k].first_key) {
                    k++;
                }else
                    break;
            }
            temp_pos = k;
            if(k >= size) temp_pos = size - 1;
            // else temp_pos = k - 1;
            uint64_t target_slot = predict_block(predicted_value,level_params[temp_pos].slope,level_params[temp_pos].intercept,level_params[temp_pos].block_num)*InnerSlotsPerBlock;
            predicted_value = target_slot;
            level --;
        }
        return int(predicted_value);
    }

    inline uint32_t predict_block (_key_t key, double slope, float intercept, uint32_t block_number) const {
        int64_t predicted_block = (key * slope + intercept + 0.5) / InnerSlotsPerBlock;
        if (predicted_block < 0)
            return 0;
        else if (predicted_block > block_number - 4)
            return block_number - 2;
        return predicted_block + 1;
    }
   


    void display_local_table(){
        std::cout<<"total :"<<local_table.size()<<"valid"<<valid_leaf<<std::endl;
        for(size_t i = 0 ; i < local_table.size() ; i++){
            if(local_table[i].valid) std::cout<<"key "<<local_table[i].key <<std::endl;
        }
    }

    void Init_load_model(){
        std::ifstream infile("/home/ming/Desktop/PLIN-N/build/local_model.txt");
        if(!infile){
            std::cerr << "Failed to open file: local_model" << std::endl;
            perror("System error");
            return ;
        }
        _key_t key;
        while(infile >> key){
            local_table.push_back({key,0,true});
        }
    }

//  private:
    void load_data(){
        keys = new _key_t[number];
        payloads = new _payload_t[number];
        std::string file_name = "Data.txt"; 
        std::ifstream infile(file_name);
        if (!infile) {
            std::cerr << "Failed to open file: " << file_name << std::endl;
            perror("System error");
            return ;
        }
        _key_t key;
        _payload_t payload;
        // char symbol;
        int count = 0;
        while (infile >> key >> payload) {
            keys[count] = key;
            payloads[count] = payload;
            count ++;
        }
        infile.close();
        std::cout<<"Loading finished!"<<std::endl;
        min_key = keys[0];
        max_key = keys[number - 1];
    }

};


