// #include <iostream>
// #include <netinet/in.h>
// #include <sys/socket.h>
// #include <unistd.h>
// #include <cstring>
// #include <csignal>
// #include <pthread.h>
// #include <random>
// #include <vector>
// #include <thread>
// #include <atomic>
// #include <iomanip>  
// #include <fstream>
// #include "plin_index.h"
// #include "serialize.h"
// #include "thread_pool.h"
// #include <unordered_map>


// #define MAX_MEM_BUFFER_SIZE 8192
// #define PORT_DEFAULT 8888
// #define MAX_CONN_LIMIT 20
// #define TEST_THREAD 40

// using TestIndex = PlinIndex;
// _key_t* keys ;
// _payload_t* payloads;
// size_t prefind_num = 2.5e5;
// size_t number = 1e7;
// size_t UPSERT_COMMAND = 1e6;
// size_t FIND_COMMAND =  1e6;
// size_t DELETE_COMMAND = 1e6;
// TestIndex test_index("No file");
// size_t error = 0 ;

// static bool should_exit = false;
// pthread_mutex_t *buffer_mutex;
// pthread_mutex_t *sockfd_mutex;

// void generate_find_command(std::string filename){
//     keys = new _key_t[number];
//     payloads = new _payload_t[number];

//     std::string file_name_ = "Data.txt"; 
//     std::ifstream infile(file_name_);

//     if (!infile) {
//         std::cerr << "Failed to open file: " << file_name_ << std::endl;
//         perror("System error");
//         return ;
//     }
//     _key_t key;
//     _payload_t payload;
//     int count = 0;
//     while (infile >> key >> payload) {
//         keys[count] = key;
//         payloads[count] = payload;
//         count ++;
//     }
//     std::cout<<count<<std::endl;
//     infile.close();
//     std::cout<<"Loading finished!"<<std::endl;

//     std::ofstream outfile(filename);
//     if (!outfile.is_open()) {
//         std::cerr << "Error opening file:"<<filename<< std::endl;
//         return ;
//     }

//     std::mt19937 search_gen(time(NULL));
//     std::uniform_int_distribution<size_t> search_dist(0,number-1);

//     std::normal_distribution<_key_t> key_dist(0, 1e10);
//     std::uniform_int_distribution<_payload_t> payload_dist(0,1e9);
//     std::mt19937 key_gen(time(NULL));
//     std::mt19937 payload_gen(time(NULL));

//     int round = 0;
//     for(size_t i = 0; i < FIND_COMMAND + UPSERT_COMMAND + DELETE_COMMAND; i++){
//         std::string command = "";   
//         size_t target_pos = search_dist(search_gen);
//         _key_t target_key = keys[target_pos];
//         command = "find " + std::to_string(target_key);
//         outfile << command <<'\n';
//     }
//     outfile<<"bye";
//     outfile.close();
// }

// void generate_bulk_load(){
//     std::ofstream outfile("Data.txt");
//     if (!outfile.is_open()) {
//         std::cerr << "Error opening file: Data.txt"<< std::endl;
//         return ;
//     }

//     std::normal_distribution<_key_t> key_dist(0, 1e10);
//     std::uniform_int_distribution<_payload_t> payload_dist(0,1e9);
//     std::mt19937 key_gen(time(NULL));
//     std::mt19937 payload_gen(time(NULL));

//     _key_t* new_keys = new _key_t[number];
//     _payload_t* new_payloads = new _payload_t[number];
//     for(size_t i = 0; i < number; i++){
//         new_keys[i] = key_dist(key_gen);
//         new_payloads[i] = payload_dist(payload_gen);
//     }
//     std::sort(new_keys, new_keys+number);
//     for (size_t i = 0; i < number; ++i) {
//         outfile << std::fixed<<new_keys[i] << " " << new_payloads[i] ;
//         if(i != number -1) outfile <<'\n';
//     }  
//     outfile.close();
// }

// void generate_delete_command(std::string filename){
//     std::ofstream outfile(filename);
//     if (!outfile.is_open()) {
//         std::cerr << "Error opening file: delete.txt"<< std::endl;
//         return ;
//     }

//     std::mt19937 search_gen(time(NULL));
//     std::uniform_int_distribution<size_t> search_dist(0,number-1);

//     for(size_t i = 0; i < 4e5; i++){ 
//         size_t target_pos = search_dist(search_gen);
//         outfile << target_pos <<'\n';
//     }
//     outfile.close();
// }

// void generate_delete_file(){
//     keys = new _key_t[number];
//     payloads = new _payload_t[number];

//     std::string file_name_ = "Data.txt"; 
//     std::ifstream infile(file_name_);

//     if (!infile) {
//         std::cerr << "Failed to open file: " << file_name_ << std::endl;
//         perror("System error");
//         return ;
//     }
//     _key_t key;
//     _payload_t payload;
//     int count = 0;
//     while (infile >> key >> payload) {
//         keys[count] = key;
//         payloads[count] = payload;
//         count ++;
//     }
//     std::cout<<count<<std::endl;
//     infile.close();
//     std::cout<<"Loading finished!"<<std::endl;

//     std::vector<std::string> file_name;
//     for (int k = 0; k < TEST_THREAD/2; k++) {
//         std::string file_ = "delete" + std::to_string(k) + ".txt";
//         file_name.push_back(file_);
//     }
//     for (int k = 0; k < TEST_THREAD/2; k++) {
//         generate_delete_command(file_name[k]);
//         sleep(2);
//     }
// }

// void generate_mix_command(std::string filename,_key_t *keys){
//     std::ofstream outfile(filename);
//     if (!outfile.is_open()) {
//         std::cerr << "Error opening file:"<<filename<< std::endl;
//         return ;
//     }
//     std::mt19937 search_gen(time(NULL));
//     std::uniform_int_distribution<size_t> search_dist(0,number-1);

//     std::normal_distribution<_key_t> key_dist(0, 1e10);
//     std::uniform_int_distribution<_payload_t> payload_dist(0,1e9);
//     std::mt19937 key_gen(time(NULL));
//     std::mt19937 payload_gen(time(NULL));

//     _key_t* new_keys = new _key_t[5*UPSERT_COMMAND];
//     _payload_t* new_payloads = new _payload_t[5*UPSERT_COMMAND];
//     for(size_t i = 0; i < 5*UPSERT_COMMAND; i++){
//         new_keys[i] = key_dist(key_gen);
//         new_payloads[i] = payload_dist(payload_gen);
//     }

//     for(size_t i = 0; i < FIND_COMMAND + 4 * UPSERT_COMMAND; i++){
//         std::string command = "";
//         if(i % 5 == 0){    
//             size_t target_pos = search_dist(search_gen);
//             outfile << "find "<<target_pos<<'\n' ;
//         }else{
//             outfile << "upsert "<<std::fixed<<new_keys[i] << " " << new_payloads[i]<<'\n' ;
//         }
//     }
//     outfile<<"bye";
//     outfile.close();
// }

// void generate_mix_file(){
//     keys = new _key_t[number];
//     payloads = new _payload_t[number];

//     std::string file_name_ = "Data.txt"; 
//     std::ifstream infile(file_name_);

//     if (!infile) {
//         std::cerr << "Failed to open file: " << file_name_ << std::endl;
//         perror("System error");
//         return ;
//     }
//     _key_t key;
//     _payload_t payload;
//     int count = 0;
//     while (infile >> key >> payload) {
//         keys[count] = key;
//         payloads[count] = payload;
//         count ++;
//     }
//     std::cout<<count<<std::endl;
//     infile.close();
//     std::cout<<"Loading finished!"<<std::endl;

//     std::vector<std::string> file_name;
//     for (int k = 0; k < TEST_THREAD; k++) {
//         std::string file_ = "command" + std::to_string(k) + ".txt";
//         file_name.push_back(file_);
//     }
//     for (int k = 0; k < TEST_THREAD; k++) {
//         generate_mix_command(file_name[k],keys);
//         sleep(2);
//     }
// }

// // void prefind_cache(_key_t* keys,_payload_t* payloads){

// //     std::string file_name = "prefind_cache.txt";
// //     std::ifstream infile(file_name);
// //     if (!infile.is_open()) {
// //         std::cerr << "Failed to open file: " << file_name << std::endl;
// //         perror("System error");
// //         exit(1);
// //     }
// //     int index;
// //     _payload_t payload;
// //     while (infile >> index) {
// //         _key_t target_key = keys[index];
// //         _payload_t answer;
// //         test_index.pre_find(target_key,answer);
// //     }
// //     infile.close();
// //     std::cout<<"Prefind finished!"<<std::endl;
// //     test_index.meta_table.train_cache(nullptr);
// // }



// void sigint_handler(int signo) {
//     should_exit = true;
//     std::cout << "The Server received Ctrl+C, will be closed\n";
// }

// void* handle_client(void *client_socket) {
//     int clientsocket = *(int *)client_socket;
//     pthread_mutex_unlock(sockfd_mutex);

//     char buffer[MAX_MEM_BUFFER_SIZE];
//     int len;
//     int index = 0;
//     uint32_t rebuild_time_temp = 0;

//     uint32_t cache_hit_count = 0;
//     uint32_t cache_miss_count = 0;
//     double cache_hit_rate = 0.0;

//     int spin_count = 0;
//     while (true) {
//         //block the plin_server during the prediction
//         while(test_index.db_logger.plin_server_block){
//             spin_count ++;
//         }


//         memset(buffer,0,MAX_MEM_BUFFER_SIZE);

//         len = recv(clientsocket, buffer, MAX_MEM_BUFFER_SIZE, 0);
//         if (len <= 0) {
//             std::cerr << "Connection lost or closed." << std::endl;
//             break;
//         }
//         buffer[len] = '\0';
//         std::string command(buffer);
//         index++;
//         if(index % 100000 == 0) std::cout<<index<<std::endl;
//         if((cache_hit_count + cache_miss_count) % 100000 == 0 &&  (cache_hit_count + cache_miss_count) != 0) 
//             std::cout<<"cache_hit_rate"<< cache_hit_count/(cache_hit_count + cache_miss_count)<<std::endl;

//         bool prehot_cache = test_index.db_logger.prehot_cache;
//         std::unordered_map<_key_t,_payload_t> hot_map_ = test_index.db_logger.hot_map_;
//         //for debug
//         //  std::unordered_map<_key_t,_payload_t> hot_map_ = test_index.db_logger.log_map_;

//         if (command == "bye" || command == "bye;") {
//             std::string message = std::to_string(pthread_self())+"Goodbye!\n";
//             send(clientsocket, message.c_str(), message.length(), 0);
//             break;
//         }else if(command.find("findid")!= std::string::npos){

//             //切分command
//             std::vector<std::string> tok;
//             size_t p = 0, q;
//             while ((q = command.find(' ', p)) != std::string::npos) {
//                 tok.push_back(command.substr(p, q - p));
//                 p = q + 1;
//             }
//             tok.push_back(command.substr(p));   // 最后一段

//             CSVRecord log_record;
//             log_record.device_id = std::stoi(tok[1]);
//             log_record.target_pos = std::stoi(tok[2]);
//             log_record.logic_id = std::stoi(tok[3]);
//             log_record.operation = "READ";
//             log_record.timestamp = get_current_timestamp_milliseconds();
//             log_record.target_key = keys[log_record.target_pos];

//             _key_t target_key = keys[log_record.target_pos];
//             _payload_t answer;
//             std::string response;
           
//             if(prehot_cache){
    
//                 if(hot_map_.find(target_key) != hot_map_.end() &&
//                     hot_map_[target_key] == payloads[log_record.target_pos]){
//                     cache_hit_count ++;
//                     response = "Success!";
//                 }else{
//                     cache_miss_count ++;
//                     int result = test_index.find_through_net(target_key,answer,log_record.logic_id);
//                     if(!result || answer!=payloads[log_record.target_pos]) response = "Failure!";
//                     else  response = "Success!";
//                 }
//             }else{
//                 int result = test_index.find_through_net(target_key,answer,log_record.logic_id);
//                 if(!result || answer!=payloads[log_record.target_pos]) response = "Failure!";
//                 else  response = "Success!";
//             }
            

//             if(response == "Success!") {
//                 log_record.payload = answer;
//                 test_index.db_logger.log_query(log_record);
//             }

//             if(test_index.rebuild_times > rebuild_time_temp && response == "Success!"){
//                 rebuild_time_temp = test_index.rebuild_times;
//                 response = "Update cache!";
//             }
//             send(clientsocket, response.c_str(), response.length(), 0);

//         }else if(command.find("cache")!= std::string::npos){
//             std::stringstream ss;
//             std::vector<std::vector<Param>>parameter = test_index.meta_table.parameter;
//             serialize_parameter(parameter, ss);
//             std::string message = ss.str();
//             send(clientsocket, message.c_str(), message.length(), 0);
//         }
//         else{
//             std::string response = "Invalid cmd or Error";
//             send(clientsocket, response.c_str(), response.length(), 0);
//         }
      
//     }
//     close(clientsocket);
//     std::cout << "Thread ID: " << pthread_self() <<"Exit"<<std::endl;
//     pthread_exit(NULL);//stop a thread
// }

// void Server(){

//     keys = new _key_t[number];
//     payloads = new _payload_t[number];

//     std::string file_name = "Data.txt"; 
//     std::ifstream infile(file_name);

//     if (!infile){
//         std::cerr << "Failed to open file: " << file_name << std::endl;
//         perror("System error");
//         return ;
//     }
//     _key_t key;
//     _payload_t payload;
//     int count = 0;
//     while (infile >> key >> payload) {
//         keys[count] = key;
//         payloads[count] = payload;
//         count ++;
//     }
//     std::cout<<count<<std::endl;
//     infile.close();
//     std::cout<<"Loading finished!"<<std::endl;

    
//     test_index.bulk_load(keys, payloads, number);
    

//     sockfd_mutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
//     pthread_mutex_init(sockfd_mutex,nullptr);

//     int server_fd, client_socket;
//     struct sockaddr_in address;

//     if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
//         fprintf(stderr, "fail to create server socket, %s\n", strerror(errno));
//         exit(EXIT_FAILURE);
//     }

//     int val = 1;
//     setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

//     address.sin_family = AF_INET;
//     address.sin_port = htons(PORT_DEFAULT);
//     address.sin_addr.s_addr = htonl(INADDR_ANY);

//     if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
//         fprintf(stderr, "fail to bind, %s\n", strerror(errno));
//         exit(EXIT_FAILURE);
//     }

//     if (listen(server_fd, MAX_CONN_LIMIT) < 0) {
//         fprintf(stderr, "fail to listen, %s\n", strerror(errno));
//         close(server_fd);
//         exit(EXIT_FAILURE);
//     }
//     signal(SIGINT, sigint_handler);

//     std::cout << "Server listening on port " << PORT_DEFAULT << "..." << std::endl;

//     while (!should_exit) {
//         struct sockaddr_in s_addr_client{};
//         int s_addr_client_len = sizeof(s_addr_client);
//         pthread_t thread_id;

//         pthread_mutex_lock(sockfd_mutex);
//         if ((client_socket = accept(server_fd, (struct sockaddr *)&s_addr_client, (socklen_t*)&s_addr_client_len)) < 0) {
//             fprintf(stderr, "fail to accept, %s\n", strerror(errno));
//             close(server_fd);
//             exit(EXIT_FAILURE);
//         }
//         std::cout << "Connection accepted." << std::endl;

//         if(pthread_create(&thread_id,nullptr,&handle_client,(void*)(&client_socket)) != 0){
//             std::cout << "Create thread fail!" << std::endl;
//             break; 
//         }
//         std::cout<<"error "<<error<<" rate"<<1.0*error/(FIND_COMMAND*TEST_THREAD*2)<<std::endl;
//     }
//     close(server_fd);
//     std::cout << "Server shutdown." << std::endl;
// }

// int main(void){
//     // generate_delete_file();
//     Server();
//     return 0;
// }