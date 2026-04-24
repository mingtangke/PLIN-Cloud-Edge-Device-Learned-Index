#include <random>
#include <fstream>
#include <ostream>
#include <math.h>
#include <algorithm>
// #include "plin_index.h"

// using TestIndex = PlinIndex;
// std::mutex cout_mutex;
// // PMAllocator * galc;

// void run_search_test(TestIndex& test_index, _key_t* keys, _payload_t* payloads, size_t number){
//     std::mt19937 search_gen(time(NULL));
//     std::uniform_int_distribution<size_t> search_dist(0,number-1);
//     for(size_t i = 0; i < 1e6; i++){
//         size_t target_pos = search_dist(search_gen);
//         _key_t target_key = keys[target_pos];
//         _payload_t answer;
//         test_index.find(target_key, answer);
//         if(answer != payloads[target_pos]){
//             std::cout<<"#Number: "<<i<<std::endl;
//             std::cout<<"#Wrong answer: "<<answer<<std::endl;
//             std::cout<<"#Correct answer: "<<payloads[target_pos]<<std::endl;
//             // test_index.find(target_key, answer);
//             // throw std::logic_error("Answer wrong!");
//         }
//     }
// }

// void run_upsert_test(TestIndex& test_index, _key_t* keys, _payload_t* payloads, size_t number){
//     std::normal_distribution<_key_t> key_dist(0, 1e10);
//     std::uniform_int_distribution<_payload_t> payload_dist(0,1e9);
//     std::mt19937 key_gen(time(NULL));
//     std::mt19937 payload_gen(time(NULL));

//     size_t upsert_times = 1e6;
//     _key_t* new_keys = new _key_t[upsert_times];
//     _payload_t* new_payloads = new _payload_t[upsert_times];
//     for(size_t i = 0; i < upsert_times; i++){
//         new_keys[i] = key_dist(key_gen);
//         new_payloads[i] = payload_dist(payload_gen);
//         test_index.upsert(new_keys[i], new_payloads[i]);
//     }
//     int error = 0;

//     for(size_t i = 0; i < upsert_times; i++){
//         _payload_t answer;
//         test_index.find(new_keys[i], answer);
//         std::cout<<answer<<std::endl;
//         if(answer != new_payloads[i]){
//             error++;
//             std::cout<<"#Number: "<<i<<std::endl;
//             std::cout<<"#Key: "<<new_keys[i]<<std::endl;
//             std::cout<<"#Wrong answer: "<<answer<<std::endl;
//             std::cout<<"#Correct answer: "<<new_payloads[i]<<std::endl;
//             // test_index.find(new_keys[i], answer);
//             // throw std::logic_error("Answer wrong!");
//         }
//     }
//     std::lock_guard<std::mutex> lock(cout_mutex);
//     std::cout<<pthread_self()<<"error_rate"<<1.0*error/upsert_times<<std::endl;

// }

// void test(uint64_t thread_cnt){
//     size_t number = 1e7;
//     std::normal_distribution<_key_t> key_dist(0, 1e10);
//     std::uniform_int_distribution<_payload_t> payload_dist(0,1e9);
//     std::mt19937 key_gen(time(NULL));
//     std::mt19937 payload_gen(time(NULL));
//     _key_t* keys = new _key_t[number];
//     _payload_t* payloads = new _payload_t[number];

//     // Generate keys and payloads
//     for(size_t i = 0; i < number; i++){
//         keys[i] = key_dist(key_gen);
//         payloads[i] = payload_dist(payload_gen);
//     }

//     // Sort keys
//     std::sort(keys, keys+number);

//     // NVM pool path
//     TestIndex test_index("no file");
//     test_index.bulk_load(keys, payloads, number);

//     // std::thread *search_test[thread_cnt];
//     // for (size_t i = 0; i < thread_cnt; i++){
//     //     search_test[i] = new std::thread(run_search_test, std::ref(test_index), keys, payloads,number);
//     // }

//     // for (size_t i = 0; i < thread_cnt; i++){
//     //     search_test[i]->join();
//     //     delete search_test[i];
//     // }
//     std::thread *upsert_test[thread_cnt];
//     for (size_t i = 0; i < thread_cnt; i++){
//         upsert_test[i] = new std::thread(run_upsert_test, std::ref(test_index), keys, payloads,number);
//     }

//     for (size_t i = 0; i < thread_cnt; i++){
//         upsert_test[i]->join();
//         delete upsert_test[i];
//     }
// }
size_t number = 1e7;
void generate_bulk_load(){
    
    std::ofstream outfile("Data2.txt");
 

    std::normal_distribution<double> key_dist(0, 1e10);
    std::uniform_int_distribution<long> payload_dist(0,1e9);
    std::mt19937 key_gen(time(NULL));
    std::mt19937 payload_gen(time(NULL));

    double* new_keys = new double[number];
    long* new_payloads = new long[number];
    for(size_t i = 0; i < number; i++){
        new_keys[i] = key_dist(key_gen);
        new_payloads[i] = payload_dist(payload_gen);
    }
    std::sort(new_keys, new_keys+number);
    for (size_t i = 0; i < number; ++i) {
        outfile << std::fixed<<new_keys[i] << " " << new_payloads[i] ;
        if(i != number -1) outfile <<'\n';
    }  
    outfile.close();
}

int main() {
   generate_bulk_load();
}