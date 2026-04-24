#pragma once
#include "utils.h"
#include <cstdlib>
#include <cassert>
#include <vector>
#include <iostream>
#include <fstream>
constexpr static uint32_t BTREE_PLUS_PAGESIZE = 256;
class page;
class btree;

struct Record{
    _key_t key;
    char *val;
    Record(_key_t k = INT64_MAX,char *v = NULL): key(k),val(v){};
    bool operator< (const Record& r) const {return key < r.key;}
};

class btree_plus{
    public:
        void insert_into_parent(page* old_node,_key_t,page *new_node);
        void btree_plus_delete_internal(_key_t,char *,uint32_t,_key_t *,bool *,page **);
    public:
        void **addr;
        page *root;

        btree_plus(void **addr,bool init);

        page* find_leaf_page(_key_t key);
        void set_new_root(page *);
        // void get_nodes_num();

        int upsert(_key_t,_payload_t);
        int insert(_key_t key,_payload_t payload);
        bool find(_key_t,_payload_t &);
        void range_query(_key_t , _key_t , std::vector<std::pair<_key_t, _payload_t>>& );
        // bool update(_key_t,_payload_t &);
        // int scan(_key_t,int,_payload_t* );
        // void range_query(_key_t lower_bound,_key_t upper_bound,std::vector<std::pair<_key_t,_payload_t>> &result);
        // void get_data(std::vector<_key_t>&keys, std::vector<_payload_t> &payloads);
        // void node_size(uint64_t & overflow_number);
        // uint32_t upsert(_key_t,_payload_t,uint32_t ds);

        bool delete_entry(_key_t key);
        void erase_node(page *node);
        bool coalesce_or_redistribute(page *node);
        bool coalesce(page **neighbor_node, page **node, page **parent, int index,bool is_left_neighbor);
        void redistribute(page *neighbor_node, page *node, page *parent, int index);
        bool adjust_root(page *old_root_node);
        void maintain_parent(page *node);

        void get_data(std::vector<_key_t>& keys, std::vector<_payload_t>& payloads);

        // void print_all();
        // void print_height();
        
        friend class page;

    public:
        // bool try_remove(_key_t,bool &need_rebuild);

        // char **find_lower(_key_t)const;

        // bool isEmpty(){
        //     return root == nullptr;
        // }

        // void clear(std::string id = "b_plus_tree");
        // void clear(void **,void *);
};

class header{
    private:
        bool is_leaf;
        bool is_root = false;
        page *next_leaf;  //8 bytes
        page *prev_leaf;
        page *parent;
        uint32_t level;
        int16_t record_num;
        int8_t is_deleted;
        uint64_t unused;

        friend class page;
        friend class btree_plus;

    public:
        header(){
            is_leaf = false;
            next_leaf = nullptr;
            prev_leaf = nullptr;
            parent = nullptr;
            level = 0;
            record_num = 0;
            is_deleted = 0;
            unused = 0;
        }
        
        ~header(){}
};

const int cardinality = (BTREE_PLUS_PAGESIZE-sizeof(header))/sizeof(Record);//the number of record in a page 14

class page{
    private:
        header hdr;
        Record records[cardinality];
        int IX_NO_PAGE = -1;
    public:
        friend class btree_plus;

        page(uint32_t level){
            hdr.level = level;
            for(int i = 0;i < cardinality;i++){
                records[i].key = FREE_FLAG;
                records[i].val = nullptr;
            }
        }
        // page(page *left,_key_t key,page *right,uint32_t level = 0):hdr(){
        //     hdr.leftmost_ptr = reinterpret_cast<page*>(left);
        //     hdr.level = level;//rising from bottom to top;
        //     records[0].key = key;
        //     records[0].val = reinterpret_cast<char*>(right);
        //     records[1].val = nullptr;
        //     hdr.last_index = 0;
        // }

        void *operator new(size_t size){
            void *ret = malloc(size);
            return ret;
        }

        inline int count(){
            int count = 0;
            for(int i = 0;i < cardinality ;i++){
                if(records[i].val != nullptr)
                    count++;
            }
            return count;
        }

        int lower_bound(_key_t target_key)  {
            // assert(hdr.record_num == count());
            if(hdr.record_num != count()){
                std::cout<<"hdr.record_num1 "<<hdr.record_num<<"count() "<<count()<<std::endl;
                exit(0);
            }
            int i;
            for(i = 0; i < hdr.record_num; i++){
                if(records[i].key >= target_key)
                    break;
            }
            return i;
        }

        int upper_bound(_key_t target_key){
            // assert(hdr.record_num == count());
            if(hdr.record_num != count()){
                std::cout<<"hdr.record_num2 "<<hdr.record_num<<"count() "<<count()<<std::endl;
                exit(0);
            }
            int i;
            for(i = 1; i < hdr.record_num; i++){
                if(records[i].key > target_key)
                    break;
            }
            return i;
        }

        bool leaf_lookup(_key_t target_key,_payload_t &payload){
            int id = lower_bound(target_key);
            int size = count();
            if(id !=  size && records[id].key == target_key){
                payload = reinterpret_cast<_payload_t>(records[id].val);    
                return true;
            }
            return false;
        }

        //用于内部结点根据key来查找该key所在的孩子结点（子树）。
        page *inner_lookup(_key_t target_key){
            int id = upper_bound(target_key) - 1;  //bug
            return reinterpret_cast<page*>(records[id].val);
        }

        int insert_in_node(_key_t key,_payload_t &payload){ //upsert
            int id = lower_bound(key);
            if(records[id].key == key && records[id].val!=nullptr) {
                records[id].val = reinterpret_cast<char*>(payload);
                payload = INT64_MAX;
                return hdr.record_num;
            }
            //move and insert
            // insert_pairs(key,payload,1); 1 2 3 4 5
            //                              1 2 3 3 4 5
            if(id < 0 || id > hdr.record_num) return hdr.record_num;

            assert(hdr.record_num < cardinality); //max is 14,15 is left to split outside
            for(int pos = hdr.record_num - 1 ; pos >= id; pos--){
                records[pos + 1].key = records[pos].key;
                records[pos + 1].val = records[pos].val;
            }
            records[id].key = key;
            records[id].val = reinterpret_cast<char*>(payload);
            hdr.record_num++;
            return hdr.record_num;
        }

        int insert_pair(int id, _key_t key ,char *val){
            if(id < 0 || id > hdr.record_num) return hdr.record_num;

            assert(hdr.record_num < cardinality);
            for(int pos = hdr.record_num - 1 ; pos >= id; pos--){
                records[pos + 1].key = records[pos].key;
                records[pos + 1].val = records[pos].val;
            }
            records[id].key = key;
            records[id].val = reinterpret_cast<char*>(val);
            hdr.record_num++;
            return hdr.record_num;
        }

        int find_child_rank(page *child){
            for(int k = 0 ; k < hdr.record_num ; k++){
                if(records[k].val == reinterpret_cast<char*>(child))
                    return k;
            }
            return -1;
        }

        page* split(page *node){
            page* sibling = new page(node->hdr.level);
            sibling->hdr.is_leaf = node->hdr.is_leaf;
            sibling->hdr.parent = nullptr;
            sibling->hdr.record_num = 0;
            
            if(sibling->hdr.is_leaf){
                page *next_node = node->hdr.next_leaf;
                if(next_node == nullptr){
                    node->hdr.next_leaf = sibling;
                    sibling->hdr.prev_leaf = node;
                    sibling->hdr.next_leaf = nullptr;
                }else{
                    sibling->hdr.prev_leaf = node;
                    node->hdr.next_leaf = sibling;
                    next_node->hdr.prev_leaf = sibling;
                    sibling->hdr.next_leaf = next_node;
                }
            }

            int right = node->hdr.record_num / 2;     // 3
            int left = node->hdr.record_num - right;  // 4
            
            //1 2 3 4 || 5 6 7
            for(int k = 0; k < right; k++){
                sibling->records[k].key = node->records[k + left].key;
                sibling->records[k].val = node->records[k + left].val;
                node->records[k+left].val = nullptr;
                sibling->hdr.record_num ++;
                node->hdr.record_num --;
            }
   
            if(!node->hdr.is_leaf) {
                for (int i = 0; i < sibling->hdr.record_num; i++) {
                    maintain_child(sibling,i);
                }
            }
            return sibling;
        }

        void maintain_child(page *node,int child_idx){
            if(!node->hdr.is_leaf){
                page* child = reinterpret_cast<page *>(node->records[child_idx].val);
                child->hdr.parent = node;
            }
            
        }

        void print_page() {
            std::cout << "Page (level " << hdr.level << "): ";
            for (int i = 0; i < hdr.record_num; ++i) {
                std::cout << records[i].key << " ";
            }
            std::cout << std::endl;
        }

        void erase_pair(int idx){
            for(int pos = idx ; pos < hdr.record_num - 1 ; pos++){ //bug
                records[pos].key = records[pos + 1].key;
                records[pos].val = records[pos + 1].val;
            }
            records[hdr.record_num - 1].val = nullptr;
            records[hdr.record_num - 1].key = FREE_FLAG;
            hdr.record_num--;
        }

        int remove(_key_t key){
            int idx = lower_bound(key);
            if(idx < 0 || idx >= hdr.record_num) return hdr.record_num;
            if(records[idx].key == key && records[idx].val!=nullptr){
                erase_pair(idx);
                return hdr.record_num;
            }
        }
    };

page * btree_plus::find_leaf_page(_key_t key){
    page *ret = root;
    // if(key == 140){
    //     std::cout<<"debug key"<<key<<std::endl;
    // }
    while(!ret->hdr.is_leaf){
        ret = ret->inner_lookup(key);
    }
    return ret;
}

bool btree_plus::find(_key_t key,_payload_t &payload){
    page *leaf = find_leaf_page(key);
    return leaf->leaf_lookup(key,payload);
}

void btree_plus::insert_into_parent(page *old_node,_key_t key,page *new_node){
    if(old_node == root){
        page *new_root = new page(0);
        new_root->hdr.is_leaf = false;
        new_root->hdr.parent = nullptr;
        new_root->hdr.record_num = 0;

        new_root->records[0].key = old_node->records[0].key;//min_key
        new_root->records[0].val = reinterpret_cast<char*>(old_node);
        new_root->records[1].key = key; //belong to right first;
        new_root->records[1].val = reinterpret_cast<char*>(new_node);
        
        old_node->hdr.parent = new_root;
        new_node->hdr.parent = new_root;
        new_root->hdr.record_num = 2;
        set_new_root(new_root);
        return;
    }

    page *father = old_node->hdr.parent;
    int child_idx = father->find_child_rank(old_node);
    new_node->hdr.parent = father;
    father->insert_pair(child_idx + 1, key , reinterpret_cast<char*>(new_node));
    if(father->hdr.record_num >= cardinality){
       page* father_sibling = father->split(father);
       insert_into_parent(father,father_sibling->records[0].key,father_sibling);
    }else{
        new_node->hdr.parent = father;  
    }
    return;

}

int btree_plus::upsert(_key_t key,_payload_t payload){
    page *leaf = find_leaf_page(key);
    int num = leaf->hdr.record_num;
    if(leaf->insert_in_node(key,payload) == num){
        if(payload == INT64_MAX) return 3;//update
        else return 0;//false
    };

    if(leaf->hdr.record_num >= cardinality){
        page* sibling = leaf->split(leaf);
        insert_into_parent(leaf,sibling->records[0].key,sibling);
    }
    return 4;//true and insertn
}

int btree_plus::insert(_key_t key,_payload_t payload){
    page *leaf = find_leaf_page(key);
    int num = leaf->hdr.record_num;
    if(leaf->insert_in_node(key,payload) == num){
        if(payload == INT64_MAX) return 3;//update
        else return 0;//false
    };

    if(leaf->hdr.record_num >= cardinality){
        page* sibling = leaf->split(leaf);
        insert_into_parent(leaf,sibling->records[0].key,sibling);
    }
    return 4;//true and insertn
}

void btree_plus::erase_node(page *node){
    if(!node->hdr.is_leaf){
        delete node;
        return ;
    }
    page *prev = node->hdr.prev_leaf;
    page *next = node->hdr.next_leaf;
    if(next == nullptr){
        prev->hdr.next_leaf = nullptr;
    }else{
        prev->hdr.next_leaf = next;
        next->hdr.prev_leaf = prev;
    }
    delete node;
    return ;
}


bool btree_plus::delete_entry(_key_t key){
    page *leaf = find_leaf_page(key);
    int old_num = leaf->hdr.record_num;
    int new_num = leaf->remove(key);
    maintain_parent(leaf);
    if(old_num != new_num) {
        coalesce_or_redistribute(leaf);
        return true;
    }else{
        return false;
    }
}

bool btree_plus::coalesce_or_redistribute(page *node){
    if(node == root){
        return adjust_root(node);
    }else if(node->hdr.record_num >= (cardinality + 1)/2 ){ //which is 7
        return false;
    }

    page* father = node->hdr.parent;
    page *neighbor;
    bool is_left_neighbor = true;
    int index = father->find_child_rank(node);
    if(index == 0){
        neighbor = reinterpret_cast<page *>(father->records[1].val);
        is_left_neighbor = false;
    }else{
        neighbor = reinterpret_cast<page *>(father->records[index-1].val);
    }

    if(node->hdr.record_num + neighbor->hdr.record_num >= (cardinality + 1)){
        redistribute(neighbor,node,father,index);
        return false;
    }else{
        coalesce(&neighbor,&node,&father,index,is_left_neighbor);
        return true;
    }
}


bool btree_plus::coalesce(page **neighbor_node, page **node, page **parent, int index,bool is_left_neighbor){
    if(index == 0){
      page *temp = *neighbor_node;
      *neighbor_node = *node;
      *node = temp; 
      index = 1; 
    }
    int size = (*neighbor_node)->hdr.record_num;
    for(int k = 0; k < (*node)->hdr.record_num; k++){
        (*neighbor_node)->insert_pair(size + k,(*node)->records[k].key,(*node)->records[k].val);
        (*neighbor_node)->maintain_child((*neighbor_node),size + k);
    }

    erase_node((*node));
    // (*parent)->erase_pair((*parent)->find_child_rank(*node));
    (*parent)->erase_pair(index);
    return coalesce_or_redistribute(*parent);
}

void btree_plus::redistribute(page *neighbor_node, page *node, page *parent, int index){

    if(index == 0){
        _key_t rise_key = neighbor_node->records[0].key;
        char *payload = neighbor_node->records[0].val;
        int insert_pos = node->hdr.record_num;
        node->insert_pair(insert_pos,rise_key,payload);
        node->maintain_child(node,insert_pos);
        neighbor_node->remove(rise_key);

        parent->records[index + 1].key = neighbor_node->records[0].key;
        // maintain_parent(neighbor_node);
        // parent->records[index + 1].val = reinterpret_cast<char*>(node); //bug
    }else{
       _key_t rise_key = neighbor_node->records[neighbor_node->hdr.record_num - 1].key;
        char *payload = neighbor_node->records[neighbor_node->hdr.record_num - 1].val;
        node->insert_pair(0,rise_key,payload);
        node->maintain_child(node,0);
        neighbor_node->remove(rise_key);

        parent->records[index].key = rise_key;
       
    }
}
bool btree_plus::adjust_root(page *old_root_node){
    if(old_root_node->hdr.is_leaf && old_root_node->hdr.record_num == 0){
        old_root_node->hdr.is_deleted = true;
        return false;
    }else if(!old_root_node->hdr.is_leaf && old_root_node->hdr.record_num == 1){
        page *new_root =  reinterpret_cast<page*>(old_root_node->records[0].val);
        new_root->hdr.parent = nullptr;
        set_new_root(new_root);
        return true;
    }
    return false;
}

void btree_plus::maintain_parent(page *node){
    
        page *curr = node;
        while (curr->hdr.parent != nullptr) {
            page *parent = curr->hdr.parent;
            int rank = parent->find_child_rank(curr);
            _key_t parent_key = parent->records[rank].key;
            _key_t child_first_key = curr->records[0].key;
            if(parent_key == child_first_key){
                break;
            }
            parent_key = child_first_key;
            curr = parent;
        }
}

void btree_plus::get_data(std::vector<_key_t>& keys, std::vector<_payload_t>& payloads){
    page* curr = root;
    while(!curr->hdr.is_leaf){
        curr = reinterpret_cast<page*>(curr->records[0].val);
    }
    while(curr != nullptr){
        for(int i = 0; i < curr->hdr.record_num; i++){
            keys.push_back(curr->records[i].key);
            payloads.push_back(reinterpret_cast<_payload_t>(curr->records[i].val));
        }
        curr = curr->hdr.next_leaf;
    }
}

void btree_plus::range_query(_key_t lower_bound, _key_t upper_bound, std::vector<std::pair<_key_t, _payload_t>>& answers){
    page* curr = find_leaf_page(lower_bound);
    while(curr != nullptr){
        for(int i = 0; i < curr->hdr.record_num; i++){
            if(curr->records[i].key >= lower_bound && curr->records[i].key <= upper_bound && curr->records[i].val!=nullptr){
                answers.push_back(std::make_pair(curr->records[i].key, reinterpret_cast<_payload_t>(curr->records[i].val)));
            }
        }
        curr = curr->hdr.next_leaf; 
    }
}

void btree_plus::set_new_root(page *new_root){
    root = new_root;
    *addr = new_root;
}

btree_plus::btree_plus(void** addr, bool init)
    : addr(addr) {
    if (init) {
        root = new page(0);
        root->hdr.is_leaf = true;
        *addr = root;
    }
    else {
        root = reinterpret_cast<page*>(*addr);
    }
}

// void test() {
//     void* addr = nullptr; 
//     btree_plus tree(&addr, true);
    
//     _payload_t payload;

//     srand(time(nullptr));
//     std::vector<int> random_keys;

//     //read from test.txt
//     std::ifstream infile("test.txt");
//     std::string line;
//     int num = 1;
//     while (std::getline(infile, line)) {
//         _key_t key = std::stod(line);
//         random_keys.push_back(key);
//         tree.upsert(key, key*2 + 1);
//     }
//     // for(int num = 1;num <= 10000;num++){
//     //     random_keys.push_back(num);
//     //     tree.insert(num, num*2 + 1);
//     // }

//     infile.close();

//     for (int k : random_keys) {
     
//         assert(tree.find(k, payload) && payload == k*2 +1);
//     }

//     int count = 0;
//     for (int k : random_keys) {
//         tree.delete_entry(k);
//         count++;
//         assert(!(tree.find(k, payload) && payload == k*2 +1));
//     }

//     std::cout << "All insert and find tests passed!" << std::endl;
// }

void generate_test_key(){
    std::ofstream outfile;
    outfile.open("test.txt");
    for(int i = 0 ; i < 10000 ; i++){
        outfile<<rand() % 10000<<std::endl;
    }
    outfile.close();
}

// // 在main函数中调用测试
// int main() {
//     test();
//     return 0;
// }

