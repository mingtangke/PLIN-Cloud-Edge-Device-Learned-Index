//
// Copyright (c) Zhou Zhang.
// Implementation of PLIN index structure
//

#pragma once

#include "inner_node.h"
#include "piecewise_linear_model.h"
#include "cache_model.h"
#include "unordered_map"
#include <map>
#include <list>
#include "hot_key.h"    

class PlinIndex {

    typedef PlinIndex SelfType;
    // For storing models, to build nodes

    struct split_log {
        LeafNode* leaf_to_split = NULL;
        LeafNode* left_sibling = NULL;
        LeafNode* right_sibling = NULL;
        LeafNode* left_node = NULL;
        LeafNode* right_node = NULL;
        // locked[0]: write lock, locked[1]: valid, locked[2]: orphan
        volatile uint32_t locked = 0;

        inline bool try_get_lock(){
            uint32_t new_value = 0;
            uint32_t old_value = __atomic_load_n(&locked, __ATOMIC_ACQUIRE);
            if (!(old_value & 1u)) {
                new_value = old_value | 1u;
                return CAS(&locked, &old_value, new_value);
            }
            return false;
        }
        
        inline bool check_lock() {
            uint32_t v = __atomic_load_n(&locked, __ATOMIC_ACQUIRE);
            if (v & 1u) {
                return false;
            }
            return true;
        }

        inline bool check_valid() {
            uint32_t v = __atomic_load_n(&locked, __ATOMIC_ACQUIRE);
            if (v & 2u) {
                return true;
            }
            return false;
        }

        inline bool check_orphan() {
            uint32_t v = __atomic_load_n(&locked, __ATOMIC_ACQUIRE);
            if (v & 4u) {
                return true;
            }
            return false;
        }

        inline void set_valid() {
            ADD(&locked, 2);
        }

        inline void set_orphan() {
            ADD(&locked, 4);
        }

        inline void release_lock() {
            __atomic_store_n(&locked, 0, __ATOMIC_RELEASE);//__ATOMIC_RELEASE
        }
    };

    struct plin_metadata {
        _key_t min_key = 0;
        _key_t max_key = 0;
        void * left_buffer = NULL;
        void * right_buffer = NULL;
        uint32_t leaf_number = 0;
        uint32_t orphan_number = 0;
        uint32_t left_buffer_number = 0;
        uint32_t right_buffer_number = 0;
        uint32_t root_number = 0;
        uint32_t level = 0;
        // Used for rebuilding, no SMO in the process of rebuilding, 1 bit lock & 31 bit read counter
        volatile uint32_t smo_lock = 0;//lock -->write ///read_lock -->31 bit read counter
        uint32_t global_version = 0;//Plin_metedata
        uint32_t rebuilding = 0;
        plin_metadata* old_plin_ = NULL;  
        InnerSlot roots[ROOT_SIZE];
        split_log logs[LOG_NUMBER];
        std::vector<LeafNode*> abandoned_nodes;
        
        inline bool get_write_lock() {
            uint32_t v;
            do {
                v = __atomic_load_n(&smo_lock, __ATOMIC_ACQUIRE);
                if (v & lockSet)
                    return false;
            } while (!CAS(&smo_lock, &v,  v | lockSet));

            // Wait until the readers all exit the critical section,此时才成功获取write_lock
            do {
                v = __atomic_load_n(&smo_lock, __ATOMIC_ACQUIRE);//31 bits read counter
            } while (v & lockMask);
            return true;//write_lock
        }

        //保证所有的split操作结束再重建inner node
        inline bool try_get_read_lock() {
            uint32_t v;
            do {
                v = __atomic_load_n(&smo_lock, __ATOMIC_ACQUIRE);
                if (v & lockSet)
                    return false;
            } while (!CAS(&smo_lock, &v,  v + 1));
            return true;
        }

        inline void release_read_lock() { SUB(&smo_lock, 1); }

        inline void release_write_lock() {
            __atomic_store_n(&smo_lock, 0, __ATOMIC_RELEASE);
        }
    };

    plin_metadata * plin_ = nullptr;
    
    class central_model:public local_model{

    public:
        size_t retrain_num = 0;
        size_t pre_find_times;
        int split_times = 0;
        std::vector<std::pair<_key_t, LeafNode *>> find_fail;
        // std::unordered_map<LeafNode *,std::vector<_key_t>> train_data;
        std::multimap<_key_t, LeafNode*> sorted_keys;

        std::atomic<bool> is_training{false};
        std::mutex train_cache_mutex;
        std::mutex LRU_train_data_mutex;
    public:
        central_model() {}

       

        void show_unconcuncy(LeafNode *most_left_leaf){
            std::ofstream outfile("unconcuncy.txt", std::ios_base::app);
            // size_t train_size = train_data.size();
            size_t local_size = local_table.size();
            // outfile<<"train_size: "<<train_size<<std::endl;
            // for(auto &i:train_data){
            //     outfile<<"key: "<<i.second[0]<<" addr: "<<i.first<<std::endl;
            // }
            outfile<<"local_size"<<local_size<<std::endl;
            for(size_t i = 0; i < local_size; i++){
                outfile<<"key: "<<local_table[i].key<<" addr: "<<local_table[i].addr<<std::endl;
            }

            if(most_left_leaf){
                outfile<<"most_left_leaf: "<<std::endl;
                while (most_left_leaf != nullptr) {
                    outfile<<"most_left_leaf: "<<most_left_leaf<<std::endl;
                    most_left_leaf = most_left_leaf->get_next();
                }
            }
            
        }

        void pess_insert_tabel(LeafNode *addr, std::vector<t_record> round){
            is_training.store(true);
            for (size_t i = 0; i < local_table.size(); i++) {
                if (local_table[i].addr == addr) {
                    local_table.erase(local_table.begin() + i);
                    break;
                }
            }
            for (size_t k = 0; k < round.size(); k++) {
                auto insert_pos = std::lower_bound(local_table.begin(), local_table.end(), round[k], 
                                                   [](const t_record& a, const t_record& b) {
                                                       return a.key < b.key;
                                                   });
                local_table.insert(insert_pos, round[k]);
            }
            is_training.store(false);
        }

        void write_split_info_to_file(const std::string& filename,LeafNode* split_addr,std::vector<t_record>new_nodes,int plin_leaf_nums,int real_leaf_num,int orphan_number) {
            std::ofstream outfile(filename, std::ios_base::app); 
            if (!outfile.is_open()) {
                std::cerr << "Error opening file: " << filename << std::endl;
                return;
            }
            
            
            outfile<< "Split Times: " << split_times << std::endl;
            outfile<<"local_table.size() "<<local_table.size()<<"plin_leaf_nums "<<plin_leaf_nums<<"real_leaf_num"<<real_leaf_num<<std::endl;
            outfile<<"orphan_number: "<<orphan_number<<"plin_leaf_nums "<<plin_leaf_nums<<"rate "<<1.0*orphan_number/plin_leaf_nums<<std::endl;
            
            outfile <<"split_Addr:"<<split_addr<<std::endl;
            outfile << "Split Round:" << std::endl;
            for (const auto& record : new_nodes) {
                outfile << "Key: " << record.key << ", Addr: " << record.addr << ", Valid: " << record.valid << std::endl;
            }
            outfile << "Local Table,Train_data,real_leaf" << std::endl;

            for(size_t k = 0 ; k < local_table.size(); k++){
                outfile<<"key: "<<std::fixed << local_table[k].key<<" addr: "<<local_table[k].addr<<'\n';
                // outfile<<" || key: "<<std::fixed <<leafmost->get_min_key()<<" addr"<<leafmost<<std::endl;
                // leafmost = leafmost->get_next();
            }
            // while(leafmost){
            //     outfile<<" || key: "<<std::fixed <<leafmost->get_min_key()<<" addr"<<leafmost<<"extra"<<std::endl;
            //     leafmost = leafmost->get_next();
            // }
            outfile.close();
            std::cout << "Split information has been written to the file." << std::endl;
        }
        
    };

    struct Segment {
        _key_t first_key;
        double slope;
        double intercept;
        uint64_t number;
        explicit Segment(const typename OptimalPiecewiseLinearModel<_key_t, size_t>::CanonicalSegment &cs)
                : first_key(cs.get_first_x()),
                number(cs.get_number()) {
            auto [cs_slope, cs_intercept] = cs.get_floating_point_segment(first_key);
            slope = cs_slope;
            intercept = cs_intercept;
        }
    };


public:
    // std::chrono::nanoseconds split_t, rebuild_t, log_t;
    // uint32_t split_times = 0;
    uint32_t rebuild_times = 0;
    central_model meta_table;
    DatabaseLogger db_logger{"../data/workload_log.csv","127.0.0.1",60001};

    PlinIndex(std::string address,std::string id = "plin", bool recovery = false) : meta_table() {
        plin_ = (plin_metadata*)malloc(sizeof(plin_metadata));
        memset(plin_,0,sizeof(plin_metadata));
        new btree_plus(&plin_->left_buffer, true);
        new btree_plus(&plin_->right_buffer, true);
        do_flush_with_double_fence(plin_,sizeof(plin_metadata));
        db_logger.start();
    }

    ~PlinIndex() {
        
    }
    
    void destroy() {
        LeafNode * cur = get_leftmost_leaf();
        LeafNode * next = cur->get_next();
        cur->destroy();//no recursion,only the methods of the leaf
        while (next) {
            cur = next;
            next = cur->get_next();
            cur->destroy();
        }
        for (uint32_t i = 0; i < plin_->root_number; i++) {
            reinterpret_cast<InnerNode*>(plin_->roots[i].ptr)->destroy();//recursion,the methods of the inner node
        }
    }

    void bulk_load(_key_t* keys, _payload_t* payloads, uint64_t number) {
        // Make segmentation for leaves
        std::vector<Segment> segments;
        db_logger.keys = keys;
        auto in_fun = [keys](auto i) {
            return std::pair<_key_t, size_t>(keys[i],i);
        };
        auto out_fun = [&segments](auto cs) { segments.emplace_back(cs); };

        uint64_t last_n = make_segmentation(number, EPSILON_LEAF_NODE, in_fun, out_fun);

        // Build leaf nodes
        uint64_t start_pos = 0;
        auto first_keys = new _key_t[last_n];
        auto leaf_nodes = new LeafNode*[last_n];  //accelerators 在当前node中，是子节点参数的副本
        auto accelerators = new InnerSlot[last_n];

        LeafNode* prev = NULL;
        for(uint64_t i = 0; i < last_n; ++i) {
            uint64_t block_number = (uint64_t) segments[i].number / (LEAF_NODE_INIT_RATIO * LeafNode::LeafRealSlotsPerBlock) + 3;//3 more
            first_keys[i] = segments[i].first_key;
            LeafNode* new_leaf = new LeafNode(accelerators[i], block_number, keys, payloads, segments[i].number, start_pos, segments[i].slope, segments[i].intercept, plin_->global_version, prev);
            leaf_nodes[i] = new_leaf;
            start_pos += segments[i].number;
            prev = leaf_nodes[i];
            meta_table.local_table.push_back({first_keys[i],new_leaf,true});
            
        //     std::vector<_key_t>data;
        //     data.push_back(first_keys[i]);
        //     meta_table.train_data.insert({new_leaf,data});
        }
    

        for(uint64_t i = 0; i < last_n - 1; ++i){
            leaf_nodes[i]->set_next(leaf_nodes[i + 1]);
        }
           
        plin_->leaf_number = last_n;
        delete [] leaf_nodes;// cannot use free,new---delete/malloc---free

        meta_table.parameter.clear();
        // Build inner nodes recursively
        uint32_t level = 0;
        uint64_t offset = 0;
        auto accelerators_tmp = accelerators;
        auto first_keys_tmp = first_keys;
        auto in_fun_rec = [&first_keys](auto i){
            return std::pair<_key_t, size_t>(first_keys[i],i);
        };
        while (level == 0 || last_n > ROOT_SIZE) {//from top to bottom
            std::vector<Param>temp;
            level++;
            offset += last_n;
            //input last_n :输入点的数量 ,output last_n :输出段数量
            //虽然 in_fun_rec 本身是一个常量，但它捕获的 first_keys 数组内容是动态变化的。
            last_n = make_segmentation(last_n, EPSILON_INNER_NODE, in_fun_rec, out_fun);
            // std::cout<<"Number of inner nodes: "<<last_n<<std::endl;
            start_pos = 0;
            first_keys = new _key_t[last_n];
            accelerators = new InnerSlot[last_n];
            for (uint64_t i = 0; i < last_n; ++i) {
                uint64_t block_number = (uint64_t) segments[offset + i].number / (INNER_NODE_INIT_RATIO * InnerNode::InnerSlotsPerBlock) + EPSILON_INNER_NODE / InnerNode::InnerSlotsPerBlock + 3;
                // uint64_t node_size_in_byte = block_number * BLOCK_SIZE + NODE_HEADER_SIZE;
                first_keys[i] = segments[offset + i].first_key;//分段的第一个关键字
                //accelerators[i] reserve the inform of the accelerators_tmp
                //first_keys_tmp是当前内部节点层的key集合
                InnerNode* new_node =  new InnerNode(accelerators[i], block_number, first_keys_tmp, accelerators_tmp, segments[offset + i].number, start_pos, segments[offset + i].slope, segments[offset + i].intercept, level,false);
                start_pos += segments[offset + i].number;
                
                Param p;
                p.first_key = first_keys[i];
                p.intercept = accelerators[i].intercept;
                p.slope = accelerators[i].slope;
                p.block_num = block_number;
                // new_node->get_parameter(p);
                temp.push_back(p);
            }
            meta_table.parameter.push_back(temp);
            temp.clear();

            delete [] accelerators_tmp;
            delete [] first_keys_tmp;
            accelerators_tmp = accelerators;//父子变换
            first_keys_tmp = first_keys;
        }

        // Build root
        plin_->min_key = keys[0];
        plin_->max_key = keys[number - 1];

        meta_table.max_key = plin_->max_key;
        meta_table.min_key = plin_->min_key;

        //train leaf_layer
        plin_->root_number = last_n;
        plin_->level = level;
        plin_->global_version = 0;
        for (uint32_t i = 0; i < ROOT_SIZE; ++i) {
            plin_->roots[i].min_key = FREE_FLAG;
        }
        for (uint32_t i = 0; i < last_n; ++i) {
            plin_->roots[i] = accelerators_tmp[i];//reserve the accelerators
        }
        do_flush(plin_, sizeof(plin_metadata));
        mfence();
        delete [] accelerators_tmp;
        delete [] first_keys_tmp;
        // show_meta_table();

        meta_table.write_split_info_to_file("split_info.txt", nullptr, std::vector<t_record>(), plin_->leaf_number, plin_->leaf_number,plin_->orphan_number);
    }

    bool find(_key_t key, _payload_t & payload) {
        if (key >= plin_->min_key && key <= plin_->max_key) {
            do {
                uint32_t i = 0;
                while (i < plin_->root_number && key >= plin_->roots[i].min_key) {
                    ++i;
                }
                InnerSlot * accelerator = &plin_->roots[--i];
                if (accelerator->type()) {
                    //accelerator->ptr就是与accelerator同一级别的slot
                    //gain innerslot,innerslot->type() = 0,说明所指向的为leaf，accelerator->ptrisleaf
                    accelerator = reinterpret_cast<InnerNode*>(accelerator->ptr)->find_leaf_node(key, accelerator);
                }
                if (accelerator->check_read_lock()) {
                    uint32_t ret = reinterpret_cast<LeafNode*>(accelerator->ptr)->find(key, payload, plin_->global_version, accelerator);
                    if (ret == 1) {
                        return true;
                    }
                    else if (ret == 0) {
                        return false;
                    }
                }
            } while (true);
        }
        // Find in buffer
        else if (key < plin_->min_key) {
            btree_plus * left_buffer = new btree_plus(&plin_->left_buffer, false);
            return left_buffer->find(key, payload);
        }
        else {
            btree_plus * right_buffer = new btree_plus(&plin_->right_buffer, false);
            return right_buffer->find(key, payload);
        }
    }

    bool find(_key_t key, _payload_t & payload,LeafNode* &leaf) {
        if (key >= plin_->min_key && key <= plin_->max_key) {
            do {
                uint32_t i = 0;
                while (i < plin_->root_number && key >= plin_->roots[i].min_key) {
                    ++i;
                }
                InnerSlot * accelerator = &plin_->roots[--i];
                if (accelerator->type()) {
                    //accelerator->ptr就是与accelerator同一级别的slot
                    //gain innerslot,innerslot->type() = 0,说明所指向的为leaf，accelerator->ptrisleaf
                    accelerator = reinterpret_cast<InnerNode*>(accelerator->ptr)->find_leaf_node(key, accelerator);
                }
                if (accelerator->check_read_lock()) {
                    uint32_t ret = reinterpret_cast<LeafNode*>(accelerator->ptr)->find(key, payload, plin_->global_version, accelerator);
                    if (ret == 1) {
                        leaf = reinterpret_cast<LeafNode*>(accelerator->ptr);
                        return true;
                    }
                    else if (ret == 0) {
                        return false;
                    }
                }
            } while (true);
        }else{
            return false;
        }
    }
    

    int find_through_net(_key_t key, _payload_t &payload ,int logic_id) {
        find(key,payload);

        if(logic_id < 0 ) logic_id = 0;
        if(logic_id > plin_->leaf_number -1) logic_id = plin_->leaf_number - 1;

        InnerSlot *accelerator = nullptr;
        LeafNode* leaf = nullptr;
        while(meta_table.is_training.load(std::memory_order_acquire)) {
        }
        leaf = meta_table.local_table[logic_id].addr;

        if (key >= plin_->min_key && key <= plin_->max_key) {
            do {
                if (leaf->check_read_lock(accelerator)) {
                    uint32_t ret = leaf->find(key, payload, plin_->global_version, accelerator);
                    return ret;
                  }
            } while (true);
        }
        return 0;
    }

    bool find_in_buffer_net(_key_t key, _payload_t & payload){
        if (key < plin_->min_key) {
            btree_plus * left_buffer = new btree_plus(&plin_->left_buffer, false);
            return left_buffer->find(key, payload);
        }
        else {
            btree_plus * right_buffer = new btree_plus(&plin_->right_buffer, false);
            return right_buffer->find(key, payload);
        }
    }
//--------------------------------------------------------------------------------//

    void range_query(_key_t lower_bound, _key_t upper_bound, std::vector<std::pair<_key_t, _payload_t>>& answers) {
        if (lower_bound < plin_->min_key) {
            btree_plus * left_buffer = new btree_plus(&plin_->left_buffer, false);
            left_buffer->range_query(lower_bound, upper_bound, answers);
            lower_bound = plin_->min_key;
        }
        if (upper_bound > plin_->max_key) {
            btree_plus * right_buffer = new btree_plus(&plin_->right_buffer, false);
            right_buffer->range_query(lower_bound, upper_bound, answers);
            upper_bound = plin_->max_key;
        }
        if (upper_bound >= plin_->min_key && lower_bound <= plin_->max_key) {
            uint32_t i = 0;
            while (i < plin_->root_number && lower_bound >= plin_->roots[i].min_key) {
                ++i;
            }
            InnerSlot * accelerator = &plin_->roots[--i];
            if (accelerator->type()) {
                accelerator = reinterpret_cast<InnerNode*>(accelerator->ptr)->find_leaf_node(lower_bound, accelerator);
            }
            reinterpret_cast<LeafNode*>(accelerator->ptr)->range_query(lower_bound, upper_bound, answers, 0, accelerator);
        }
    }

    void upsert(_key_t key, _payload_t payload) {
        if (key > plin_->min_key && key < plin_->max_key) {
            uint32_t ret;
            do {
                uint32_t i = 0;
                while (i < plin_->root_number && key >= plin_->roots[i].min_key) {
                    ++i;
                }
                InnerSlot * accelerator = &plin_->roots[--i];
                
                //accelerator->type() = 1 说明accelerator的下一层，accelerator->ptr是内部节点,
                if (accelerator->type()) {
                    accelerator = reinterpret_cast<InnerNode*>(accelerator->ptr)->find_leaf_node(key, accelerator);
                }
                LeafNode * leaf_to_split;
                // ret = 1 : update in a slot; ret = 2 : insert in a free slot; ret = 3 : update in overflow block; ret = 4 : insert in overflow block; 
                // ret = 5 : insert in overflow block & need to split; ret = 6 : insert in overflow block & need to split orphan node; ret = 7 : the node is locked
                // ret = reinterpret_cast<LeafNode*>(accelerator->ptr)->upsert(key, payload, plin_->global_version, leaf_to_split, accelerator);

                LeafNode * leaf = reinterpret_cast<LeafNode*>(accelerator->ptr);
                if(!leaf ||!leaf->check_split_lock()||plin_->rebuilding){
                    ret = 7;
                    continue;
                }

                ret = reinterpret_cast<LeafNode*>(accelerator->ptr)->upsert(key, payload, plin_->global_version, leaf_to_split, accelerator);

                // Split leaf node
                if (ret == 5) {
                    #ifdef BACKGROUND_SPLIT
                        //leaf
                        std::thread split_thread(&SelfType::split, this, leaf_to_split, accelerator);
                        split_thread.detach();
                    #else
                        split(leaf_to_split, accelerator);
                    #endif
                }
                else if (ret == 6) {
                    #ifdef BACKGROUND_SPLIT
                        std::thread split_thread(&SelfType::split, this, leaf_to_split, nullptr);
                        split_thread.detach();
                    #else
                        split(leaf_to_split, NULL);
                    #endif
                }
            } while (ret == 7);
        }
        // Upsert in buffer
        else if (key < plin_->min_key) {
            btree_plus * left_buffer = new btree_plus(&plin_->left_buffer, false);
            uint32_t ret = left_buffer->upsert(key, payload);
            if (ret == 4) {
                // TODO: merge buffer,havent be realised!
                if (++plin_->left_buffer_number > MAX_BUFFER) {}
            }
        }
        else {
            btree_plus * right_buffer = new btree_plus(&plin_->right_buffer, false);
            uint32_t ret = right_buffer->upsert(key, payload);
            if (ret == 4) {
                // TODO: merge buffer,TODO: merge buffer,havent be realised!
                if (++plin_->right_buffer_number > MAX_BUFFER) {}
            }
        }
    }

    void remove (_key_t key) {
        if (key > plin_->min_key && key < plin_->max_key) {
            uint32_t ret;
            do {
                uint32_t i = 0;
                while (i < plin_->root_number && key >= plin_->roots[i].min_key) {
                    ++i;
                }
                InnerSlot * accelerator = &plin_->roots[--i];
                if (accelerator->type()) {
                    accelerator = reinterpret_cast<InnerNode*>(accelerator->ptr)->find_leaf_node(key, accelerator);
                }
                ret = reinterpret_cast<LeafNode*>(accelerator->ptr)->remove(key, plin_->global_version, accelerator);
            } while (ret == 3);
        }
        else if (key < plin_->min_key) {
            btree_plus * left_buffer = new btree_plus(&plin_->left_buffer, false);
            if (left_buffer->delete_entry(key)) {
                --plin_->left_buffer_number;
            }
        }
        else {
            btree_plus * right_buffer = new btree_plus(&plin_->right_buffer, false);
            if (right_buffer->delete_entry(key)) {
                --plin_->right_buffer_number;
            }
        }
    }

    InnerSlot* get_parent(const InnerSlot * node){//parent root
        uint32_t i = 0;
        while (i < plin_->root_number && node->min_key >= plin_->roots[i].min_key) {
            ++i;
        }
        --i;
        return &plin_->roots[i];
    }
    
    // void debug_for_keys(_key_t *keys,size_t number){
    //     std::ofstream outfile("debug_for_keys.txt", std::ios_base::app);
    //     if (!outfile.is_open()) {
    //         std::cerr << "Error opening file: " << "debug_for_keys.txt" << std::endl;
    //         return;
    //     }
    //     outfile<<"split_time"<<meta_table.split_times<<std::endl;
    //     for(size_t k = 0; k < number; k++){
    //         outfile<<keys[k]<<std::endl;
    //     }
    //     outfile.close();
    // }

    // Split & insert nodes
    void split (LeafNode * leaf_to_split, InnerSlot * accelerator = nullptr) { 
        // std::chrono::_V2::system_clock::time_point start_time = std::chrono::system_clock::now();

        LeafNode * left_sibling = leaf_to_split->get_prev();
        LeafNode * right_sibling = leaf_to_split->get_next();
        // lock在做的时候已经被获取了，并发没有问题
        // Check wether the index is rebuilding, no smo in rebuilding process
        if(!plin_->try_get_read_lock()) {
            return;
        }
        // Get split lock of the node, the prev node, and the next node
        if (!leaf_to_split->try_get_split_lock()) {
            plin_->release_read_lock();
            return;
        }   
        if ((!left_sibling) || (!left_sibling->try_get_split_lock())) {
            leaf_to_split->release_lock();
            plin_->release_read_lock();
            return;
        }
        if ((!right_sibling) || (!right_sibling->try_get_split_lock())) {
            if (left_sibling)
                left_sibling->release_lock();
            leaf_to_split->release_lock();
            plin_->release_read_lock();
            return;
        }
        //一步没获得锁，全部重新来
        if (accelerator) {
            accelerator->get_write_lock();
        }
        else {
            leaf_to_split->get_write_lock();
        }

        // std::chrono::_V2::system_clock::time_point start_log_time = std::chrono::system_clock::now();
        // Write log
        uint32_t log_number = LOG_NUMBER;
        do {
            for (uint32_t i = 0; i < LOG_NUMBER; ++i) {//找空隙
                if (plin_->logs[i].try_get_lock()) {
                    log_number = i;
                    break;
                }
            }   
        } while (log_number == LOG_NUMBER);
        plin_->logs[log_number].leaf_to_split = leaf_to_split;
        if (left_sibling)
            plin_->logs[log_number].left_sibling = left_sibling;
        else
            plin_->logs[log_number].left_sibling = NULL;
        if (right_sibling)
            plin_->logs[log_number].right_sibling = right_sibling;//orphan is contained
        else
            plin_->logs[log_number].right_sibling = NULL;
        // std::chrono::_V2::system_clock::time_point end_log_time = std::chrono::system_clock::now();
        
        
        std::vector<_key_t> keys;
        std::vector<_payload_t> payloads;
        // Merge & sort data in the node
        leaf_to_split->get_data(keys, payloads, plin_->global_version);

//---------------------------------tt----------------------------------------//
        LeafNode *temp_leaf_to_split = leaf_to_split; 
//---------------------------------retrain------------------------------------//

        std::vector<Segment> segments;
        auto in_fun = [keys](auto i) {
            return std::pair<_key_t, size_t>(keys[i],i);
        };
        auto out_fun = [&segments](auto cs) { segments.emplace_back(cs); };
        // Train models
        uint64_t last_n = make_segmentation(keys.size(), EPSILON_LEAF_NODE, in_fun, out_fun);
        uint64_t start_pos = 0;
        auto leaf_nodes = new LeafNode*[last_n];
        auto accelerators = new InnerSlot[last_n];
        LeafNode * prev = left_sibling;
        // Build leaf nodes

        // debug_for_keys(&keys[0],keys.size()); //for debug
        
        std::vector<t_record>new_nodes;
        for (uint64_t i = 0; i < last_n; ++i) {
            uint64_t block_number = (uint64_t) segments[i].number / (LEAF_NODE_INIT_RATIO * LeafNode::LeafRealSlotsPerBlock) + 3;
            leaf_nodes[i] = new LeafNode(accelerators[i], block_number, &keys[0], &payloads[0], segments[i].number, start_pos, segments[i].slope, segments[i].intercept, plin_->global_version, prev);
            prev = leaf_nodes[i];
            new_nodes.push_back({keys[start_pos],leaf_nodes[i],true}); //bug keys[i+start_pos]
            start_pos += segments[i].number; //segments[i].number == 1，这是空的()，居然是空的，我真的是无语了，居然有空数据
            //还是溢出树的事情，有空数据的话那我就直接在Merge_Sort里面舍去空的数据，方便跑，后续我把overflow的数据改成B+树
        }
        // print meta_table.local_table.size();
        meta_table.valid_leaf += last_n-1;

        for (uint64_t i = 0; i < last_n - 1; ++i){
            leaf_nodes[i]->set_next(leaf_nodes[i + 1]);
        }
        leaf_nodes[last_n - 1]->set_next(right_sibling);

        
        //leaf有加速器的话，从父亲节点获取，不必访问自身
        if (accelerator) {
            accelerator->get_read_lock();
        }
        else {
            leaf_to_split->get_read_lock();
            plin_->logs[log_number].set_orphan();
        }
        //当插入的时候才不允许读
        plin_->logs[log_number].left_node = leaf_nodes[0];
        plin_->logs[log_number].right_node = leaf_nodes[last_n - 1];
        plin_->logs[log_number].set_valid();
        do_flush(&plin_->logs[log_number], sizeof(split_log));
        mfence();

        if (left_sibling)
            left_sibling->set_next(leaf_nodes[0]);//处理连接极左和/右边
        if (right_sibling)
            right_sibling->set_prev(leaf_nodes[last_n - 1]);
        delete [] leaf_nodes;

        // leaf_to_split->destroy(); delay
        leaf_to_split->set_abandoned();
        if(plin_->abandoned_nodes.size() >= 2){
            plin_->abandoned_nodes.erase(plin_->abandoned_nodes.begin());
            plin_->abandoned_nodes.push_back(leaf_to_split);
        }
        plin_->abandoned_nodes.push_back(leaf_to_split);

        
        // Insert leaf nodes
        if(accelerator) {
            for (uint64_t i = 0; i < last_n; ++i) {
                int ret = upsert_node(accelerators[i]);//非孤儿，有父亲slots,插入父节点,说明是非孤儿节点分裂，未每个造父亲
                // if(ret == 1){
                //     std::cout<<"update!"<<std::endl;
                //     std::cout<<accelerators[i].ptr<<std::endl;
                // }else if(ret == 3){
                //     std::cout<<"insert node failure!"<<std::endl;
                //     std::cout<<accelerators[i].ptr<<std::endl;
                // }
            }
        }
        else {
            plin_->orphan_number += last_n - 1;
        }
        ////-----------------------------------------------------------------//
        int debug_leaf_num = debug();

        meta_table.pess_insert_tabel(temp_leaf_to_split,new_nodes);
        meta_table.write_split_info_to_file("split_info.txt",temp_leaf_to_split,new_nodes,plin_->leaf_number,debug_leaf_num,plin_->orphan_number);
        new_nodes.clear();


        delete [] accelerators;
        
        //add to deal with dead_lock
        if (accelerator) {
            accelerator->release_lock();
        }
        else {
            leaf_to_split->release_lock();
        }

        plin_->release_read_lock();
        if (left_sibling)
            left_sibling->release_lock();
        if (right_sibling)
            right_sibling->release_lock();
        plin_->logs[log_number].release_lock();

        // std::chrono::_V2::system_clock::time_point end_time = std::chrono::system_clock::now();

        // log_t += std::chrono::duration_cast<std::chrono::nanoseconds>(end_log_time - start_log_time);
        // split_t += std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);
        meta_table.split_times++;

        if (double(plin_->orphan_number) / plin_->leaf_number > MAX_ORPHAN_RATIO) {
            rebuild_inner_nodes();
        }
    }
    
    inline int debug(){
        LeafNode* node = get_leftmost_leaf();
        int num = 0;
        //1 2 3 4 5
        while(node){
            num++;
            node = node->get_next();
        }
        return num;
    }

    //重建的时候不用插入直接重新生成就可以了，这个函数用在split
    int upsert_node (InnerSlot& node) {
        uint32_t i = 0;
        while (i < plin_->root_number && node.min_key >= plin_->roots[i].min_key) {
            ++i;
        }
        --i;
        uint32_t ret = reinterpret_cast<InnerNode*>(plin_->roots[i].ptr)->upsert_node(node, &plin_->roots[i]);
        if (ret > 1){
            ++plin_->leaf_number;//insert not update
        }
        if (ret > 2){
            ++plin_->orphan_number;//unfinihed
        }
        return ret;
    }

    LeafNode* get_leftmost_leaf() {
        return reinterpret_cast<InnerNode*>(plin_->roots[0].ptr)->get_leftmost_leaf();
    }

    // Rebuild inner nodes, allow insert and search, no SMO
    void rebuild_inner_nodes() {
        //for debug
        std::cout<<"split_time rebuild inner_nodes"<<std::endl;

        std::chrono::_V2::system_clock::time_point start_time = std::chrono::system_clock::now();

        if(!plin_->get_write_lock())
            return;

        uint64_t last_n = plin_->leaf_number;
        auto first_keys = new _key_t[last_n];
        auto accelerators = new InnerSlot[last_n];
        LeafNode* node = get_leftmost_leaf();
        
        for (uint64_t i = 0; i < last_n; ++i) {
            node->get_info(first_keys[i], accelerators[i]);
            node = node->get_next();
        }


        std::vector<Segment> segments;
        uint32_t level = 0;
        uint64_t offset = 0;
        auto accelerators_tmp = accelerators;//get inform
        auto first_keys_tmp = first_keys;
        auto in_fun_rec = [&first_keys](auto i){
            return std::pair<_key_t, size_t>(first_keys[i],i);
        };
        auto out_fun = [&segments](auto cs) { segments.emplace_back(cs); };
        
        meta_table.parameter.clear();
        
        while (level == 0 || last_n > ROOT_SIZE) {
            std::vector<Param>temp;
            level++;    
            last_n = make_segmentation(last_n, EPSILON_INNER_NODE, in_fun_rec, out_fun);

            std::cout<<"Number of inner nodes: "<<last_n<<std::endl;

            uint64_t start_pos = 0;
            first_keys = new _key_t[last_n];
            accelerators = new InnerSlot[last_n];
            for (uint64_t i = 0; i < last_n; ++i) {
                uint64_t block_number = (uint64_t) segments[offset + i].number / (INNER_NODE_INIT_RATIO * InnerNode::InnerSlotsPerBlock) + EPSILON_INNER_NODE / InnerNode::InnerSlotsPerBlock + 2;
                // uint64_t node_size_in_byte = block_number * BLOCK_SIZE + NODE_HEADER_SIZE;
                first_keys[i] = segments[offset + i].first_key;
               
                InnerNode *new_node = new InnerNode(accelerators[i], block_number, first_keys_tmp, accelerators_tmp, segments[offset + i].number, start_pos, segments[offset + i].slope, segments[offset + i].intercept, level, true);
                start_pos += segments[offset + i].number;
                Param p;
                p.first_key = first_keys[i];
                p.intercept = accelerators[i].intercept;
                p.slope = accelerators[i].slope;
                p.block_num = block_number;
                temp.push_back(p);
            }
            meta_table.parameter.push_back(temp);
            temp.clear();

            delete [] accelerators_tmp;
            delete [] first_keys_tmp;
            accelerators_tmp = accelerators;
            first_keys_tmp = first_keys;
            offset += last_n;
        }
        plin_->rebuilding = 1;
        plin_metadata * old_plin_ = (plin_metadata *)malloc(sizeof(plin_metadata));
        plin_->old_plin_ = old_plin_;
        old_plin_->root_number = plin_->root_number;
        for (uint32_t i = 0; i < plin_->root_number; ++i)
            old_plin_->roots[i] = plin_->roots[i];
        old_plin_->level = plin_->level;
        old_plin_->orphan_number = plin_->orphan_number;
        do_flush(old_plin_, sizeof(plin_metadata));
        mfence();

        // Build new root
        plin_->rebuilding = 2;
        plin_->root_number = last_n;
        plin_->level = level;
        plin_->orphan_number = 0;
        for (uint32_t i = 0; i < last_n; ++i) {
            plin_->roots[i] = accelerators_tmp[i];//the last one
        }
        for (uint32_t i = last_n; i < ROOT_SIZE; ++i) {
            plin_->roots[i].min_key = FREE_FLAG;
        }
        plin_->rebuilding = 0;
        rebuild_times++;

        delete [] accelerators_tmp;
        delete [] first_keys_tmp;
        rebuild_times++;
        
        std::this_thread::sleep_for(std::chrono::seconds(3));  //for bug

        for (uint32_t i = 0; i < old_plin_->root_number; i++) {
           reinterpret_cast<InnerNode*>(old_plin_->roots[i].ptr)->destroy();
        }

        free(old_plin_);
        plin_->old_plin_ = NULL;
        plin_->release_write_lock();

        std::chrono::_V2::system_clock::time_point end_time = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);

        std::ofstream outfile("rebuild_time.txt", std::ios_base::app);
        if (outfile.is_open()) {
            outfile<<"plin_->leaf_number"<<plin_->leaf_number<<std::endl;
            outfile << "Rebuild inner nodes duration: " << duration.count() << " nanoseconds" << std::endl;
            outfile.close();
        } else {
            std::cerr << "Error opening file: execution_time.txt" << std::endl;
        }
    }
};
