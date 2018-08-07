// Copyright (c) 2018 Baidu, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <unordered_map>
#include <bthread/mutex.h>
#include "proto/meta.interface.pb.h"
#include "meta_server.h"
#include "meta_state_machine.h"

namespace baikaldb {
struct InstanceStateInfo {
    int64_t timestamp; //上次收到该实例心跳的时间戳
    pb::Status state; //实例状态
};

struct Instance {
    std::string address;
    int64_t capacity;
    int64_t used_size;
    //std::vector<int64_t> regions;
    std::string resource_tag;
    //std::string logic_room;
    std::string physical_room;
    InstanceStateInfo instance_status;
    Instance() {
       instance_status.state = pb::NORMAL;
       instance_status.timestamp = butil::gettimeofday_us();
    }
    Instance(const pb::InstanceInfo& instance_info) : 
        address(instance_info.address()),
        capacity(instance_info.capacity()),
        //若请求中没有该字段，为了安全起见
        used_size(instance_info.capacity()),
        resource_tag(instance_info.resource_tag()),
        physical_room(instance_info.physical_room()) {
        if (instance_info.has_used_size()) {
            used_size = instance_info.used_size();
        }
        instance_status.state = pb::NORMAL;
        instance_status.timestamp = butil::gettimeofday_us(); 
    }
};

class ClusterManager {
public:
    ~ClusterManager() {
        bthread_mutex_destroy(&_physical_mutex);
        bthread_mutex_destroy(&_instance_mutex);
    }
    static ClusterManager* get_instance() {
        static ClusterManager instance;
        return &instance;
    }
    friend class QueryClusterManager;
    void process_cluster_info(google::protobuf::RpcController* controller,
                              const pb::MetaManagerRequest* request, 
                              pb::MetaManagerResponse* response,
                              google::protobuf::Closure* done);
    void add_logical(const pb::MetaManagerRequest& request, braft::Closure* done);
    void drop_logical(const pb::MetaManagerRequest& request, braft::Closure* done);

    void add_physical(const pb::MetaManagerRequest& request, braft::Closure* done); 
    void drop_physical(const pb::MetaManagerRequest& request, braft::Closure* done);
   
    void add_instance(const pb::MetaManagerRequest& request, braft::Closure* done); 
    void drop_instance(const pb::MetaManagerRequest& request, braft::Closure* done); 
    void update_instance(const pb::MetaManagerRequest& request, braft::Closure* done);

    void move_physical(const pb::MetaManagerRequest& request, braft::Closure* done); 
    
    void set_instance_dead(const pb::MetaManagerRequest* request,
                             pb::MetaManagerResponse* response,
                             uint64_t log_id); 
    
    void process_instance_heartbeat_for_store(const pb::InstanceInfo& request);
    void process_peer_heartbeat_for_store(const pb::StoreHeartBeatRequest* request, 
                pb::StoreHeartBeatResponse* response);
    void store_healthy_check_function();
    //从集群中选择可用的实例
    //排除状态不为normal, 如果输入有resource_tag会优先选择resource_tag
    //排除exclued
    int select_instance_rolling(const std::string& resource_tag, 
                        const std::set<std::string>& exclude_stores, 
                        std::string& selected_instance);
    int select_instance_min(const std::string& resource_tag,
                            const std::set<std::string>& exclude_stores,
                            int64_t table_id,
                            std::string& delected_instance);
    void load_snapshot();
public:
    int64_t get_instance_count(const std::string& resource_tag) {
        int64_t count = 0; 
        BAIDU_SCOPED_LOCK(_instance_mutex);
        for (auto& instance_info : _instance_info) {
            if (instance_info.second.resource_tag == resource_tag) {
                ++count;
            }
        }
        return count;
    }
    
    int64_t get_peer_count(int64_t table_id) {
        int64_t count = 0;
        BAIDU_SCOPED_LOCK(_instance_mutex);
        for (auto& region_count: _instance_regions_count_map) {
            if (region_count.second.find(table_id) != region_count.second.end()) {
                count += region_count.second[table_id];
            }
        }
        return count;
    }

    int64_t get_peer_count(const std::string& instance, int64_t table_id) {
        BAIDU_SCOPED_LOCK(_instance_mutex);
        if (_instance_regions_count_map.find(instance) == _instance_regions_count_map.end()
                || _instance_regions_count_map[instance].find(table_id) == _instance_regions_count_map[instance].end()) {
            return 0;
        }
        return _instance_regions_count_map[instance][table_id];
    }

    void sub_peer_count(const std::string& instance, int64_t table_id) {
        BAIDU_SCOPED_LOCK(_instance_mutex);
        if (_instance_regions_count_map.find(instance) == _instance_regions_count_map.end()
                || _instance_regions_count_map[instance].find(table_id) == _instance_regions_count_map[instance].end()) {
            return ;
        }
        _instance_regions_count_map[instance][table_id]--;
    }
    //切主时主动调用，恢复状态为正常
    void reset_instance_status() {
        BAIDU_SCOPED_LOCK(_instance_mutex);
        for (auto& instance_pair : _instance_info) {
            instance_pair.second.instance_status.state = pb::NORMAL;
            instance_pair.second.instance_status.timestamp = butil::gettimeofday_us();
            _instance_regions_map[instance_pair.first] = 
                    std::unordered_map<int64_t, std::vector<int64_t>>{};
            _instance_regions_count_map[instance_pair.first] = 
                    std::unordered_map<int64_t, int64_t>{};
        }
    }
  
    void set_instance_regions(const std::string& instance, 
                    const std::unordered_map<int64_t, std::vector<int64_t>>& instance_regions,
                    const std::unordered_map<int64_t, int64_t>& instance_regions_count) {
        BAIDU_SCOPED_LOCK(_instance_mutex);
        _instance_regions_map[instance] = instance_regions;
        _instance_regions_count_map[instance] = instance_regions_count;
    }
    
    int update_instance_info(const pb::InstanceInfo& instance_info) {
        std::string instance = instance_info.address();
        BAIDU_SCOPED_LOCK(_instance_mutex);
        if (_instance_info.find(instance) == _instance_info.end()) {
            return -1;
        }
        _instance_info[instance].capacity = instance_info.capacity();
        _instance_info[instance].used_size = instance_info.used_size();
        _instance_info[instance].resource_tag = instance_info.resource_tag();
        _instance_info[instance].instance_status.state = pb::NORMAL;
        _instance_info[instance].instance_status.timestamp = butil::gettimeofday_us();
        return 0;
    }
    
    int set_dead_for_instance(const std::string& dead_instance) {
        BAIDU_SCOPED_LOCK(_instance_mutex);
        if (_instance_info.find(dead_instance) == _instance_info.end()) {
            return -1;
        }
        //修改该实例上报心跳的时间，是健康检查线程可以直接判定该实例为dead
        _instance_info[dead_instance].instance_status.timestamp = 0;
        _instance_info[dead_instance].instance_status.state = pb::DEAD;
        return 0;
    }
    
    void set_meta_state_machine(MetaStateMachine* meta_state_machine) {
        _meta_state_machine = meta_state_machine;
    }
private:
    ClusterManager() {
        bthread_mutex_init(&_physical_mutex, NULL);
        bthread_mutex_init(&_instance_mutex, NULL);
    }
    bool whether_legal_for_select_instance(
                const std::string& candicate_instance,
                const std::string& resource_tag,
                const std::set<std::string>& exclude_stores);
    std::string construct_logical_key() {
        return MetaServer::CLUSTER_IDENTIFY
                + MetaServer::LOGICAL_CLUSTER_IDENTIFY
                + MetaServer::LOGICAL_KEY;
    }
    std::string construct_physical_key(const std::string& logical_key) {
        return MetaServer::CLUSTER_IDENTIFY
                + MetaServer::PHYSICAL_CLUSTER_IDENTIFY
                + logical_key;
    }
    std::string construct_instance_key(const std::string& instance) {
        return MetaServer::CLUSTER_IDENTIFY
                + MetaServer::INSTANCE_CLUSTER_IDENTIFY
                + instance;
    }
    void load_instance_snapshot(const std::string& instance_prefix,
                                 const std::string& key, 
                                 const std::string& value);
    void load_physical_snapshot(const std::string& physical_prefix,
                                 const std::string& key, 
                                 const std::string& value);
    void load_logical_snapshot(const std::string& logical_prefix,
                                const std::string& key, 
                                const std::string& value);
private:
    //std::mutex                                                  _physical_mutex;
    bthread_mutex_t                                                  _physical_mutex;
    //物理机房与逻辑机房对应关系 , key:物理机房， value:逻辑机房
    std::unordered_map<std::string, std::string>                _physical_info;
    //物理机房与逻辑机房对应关系 , key:逻辑机房， value:物理机房组合
    std::unordered_map<std::string, std::set<std::string>>      _logical_physical_map;
    
    //std::mutex                                                  _instance_mutex;
    bthread_mutex_t                                                  _instance_mutex;
    //物理机房与实例对应关系, key:实例， value:物理机房
    //std::unordered_map<std::string, std::string>                _instance_physical_map;
    //物理机房与实例对应关系, key:物理机房， value:实例
    //std::unordered_map<std::string, std::set<std::string>>      _physical_instance_map;
    //实例信息
    std::unordered_map<std::string, Instance>                   _instance_info;
    std::string                                                 _last_rolling_instance;

    //下边信息只在leader中保存，切换leader之后需要一段时间来收集数据，会出现暂时的数据不准情况
    typedef std::unordered_map<int64_t, std::vector<int64_t>>      TableRegionMap;
    typedef std::unordered_map<int64_t, int64_t>                TableRegionCountMap;
    //每个实例上，保存的每个表的哪些reigon
    std::unordered_map<std::string, TableRegionMap>             _instance_regions_map; 
    //每个实例上。保存每个表的region的个数
    std::unordered_map<std::string, TableRegionCountMap>        _instance_regions_count_map;

    MetaStateMachine*                                           _meta_state_machine = NULL;
}; //class ClusterManager

}//namespace

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */