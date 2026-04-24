//
// Copyright (c) Zhou Zhang.
//


#pragma once

#include "parameters.h"
#include <chrono>
#include <ctime>

#define CAS(_p, _u, _v) (__atomic_compare_exchange_n(_p, _u, _v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
#define ADD(_p, _v) (__atomic_add_fetch(_p, _v, __ATOMIC_ACQUIRE))
#define SUB(_p, _v) (__atomic_sub_fetch(_p, _v, __ATOMIC_ACQUIRE))



struct InnerSlot {
    _key_t min_key;
    void * ptr;
    double slope;
    float intercept;

    // info[31]: write lock, info[30]: read lock, info[29]: type, info[28:0]: block_number
    uint32_t info;
    // bool split = false;

    inline uint32_t block_number() const{
        return info & 0x1fffffffu;
    }

    inline uint32_t type() const{
        return (info & 0x3fffffffu) >> 29;
    }

    inline void set_block_number(uint32_t block_number){
        assert(block_number < (1 << 29));
        info = (info & 0xe0000000u) | (block_number & 0x1fffffffu) ;
    }
    
    inline void set_type(uint32_t type){
        info = (info & (~(1u << 29))) | (type << 29) ;
    }
    
    inline void init_lock(){
        info = info & (~(3u << 30));
    }

    inline void get_lock() {
        uint32_t old_value, new_value;
        do {
            while (true) {
                old_value = __atomic_load_n(&info, __ATOMIC_ACQUIRE);
                if (!(old_value & lockSet)){
                    break;
                }
            }
            new_value = old_value | lockSet;
        } while (!CAS(&info, &old_value, new_value));
        /*
        CAS(&info, &old_value, new_value) 用于尝试将 new_value 写入 info，
        前提是 info 的当前值等于 old_value。如果成功，表示当前线程成功获取了锁；
        如果失败，表示在此期间 info 的值已被其他线程修改，循环将重新开始，
        继续尝试获取锁。*/
    }

    inline void get_write_lock() {
        __atomic_store_n((uint32_t *)&info, info | lockSet, __ATOMIC_RELEASE);
    }

    inline void get_read_lock() {
        __atomic_store_n((uint32_t *)&info, info | second_bit_lockSet, __ATOMIC_RELEASE);
    }

    inline bool check_write_lock() const {
        uint32_t v = __atomic_load_n(&info, __ATOMIC_ACQUIRE);
        if (v & lockSet) {
            return false;
        }
        return true;
    }

    inline bool check_read_lock() const {
        uint32_t v = __atomic_load_n(&info, __ATOMIC_ACQUIRE);
        if (v & second_bit_lockSet) {
            return false;
        }
        return true;
    }

    inline void release_lock() {
        __atomic_store_n((uint32_t *)&info, info & double_bit_lockMask, __ATOMIC_RELEASE);
    }
};

static void model_correction(double& slope, float& intercept, uint64_t size, _key_t min_key, _key_t max_key) {
    double min_pos = min_key * slope + intercept;
    double max_pos = max_key * slope + intercept + 1;
    if (min_pos > 0 && max_pos < size - 1) {
        slope = (size - 1) / (max_key - min_key);
        intercept = 0 - slope * min_key;
    }
    else if (min_pos > 0) {
        slope = max_pos / (max_key - min_key);
        intercept = 0 - slope * min_key;
    }
    else if (max_pos < size) {
        slope = (size - min_pos - 1) / (max_key - min_key);
        intercept = size - 1 - slope * max_key;
    }
}


struct CSVRecord {
    double timestamp;
    int device_id;
    int target_pos;
    std::string operation;
    int logic_id;
    double target_key;
    uint64_t payload;
};


long long get_current_timestamp_seconds() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
}