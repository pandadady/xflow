#include <algorithm>
#include <ctime>
#include <iostream>

#include <mutex>
#include <functional>
#include <random>
#include <string>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <immintrin.h>

#include "src/io/load_data_from_local.h"
#include "src/base/thread_pool.h"
#include "ps/ps.h"

namespace xflow{
class FMWorker{
 public:
  FMWorker(const char *train_file,
           const char *test_file) :
           train_file_path(train_file),
           test_file_path(test_file) {
    kv_w = new ps::KVWorker<float>(0);
    kv_v = new ps::KVWorker<float>(1);
    base_ = new Base;
    core_num = std::thread::hardware_concurrency();
    pool_ = new ThreadPool(core_num);
  }
  ~FMWorker() {}

  void calculate_pctr(int start, int end){
    auto all_keys = std::vector<Base::sample_key>();
    auto unique_keys = std::vector<ps::Key>();
    int line_num = 0;
    for(int row = start; row < end; ++row) {
      int sample_size = test_data->fea_matrix[row].size();
      Base::sample_key sk;
      sk.sid = line_num;
      for(int j = 0; j < sample_size; ++j) {
        size_t idx = test_data->fea_matrix[row][j].fid;
        sk.fid = idx;
        all_keys.push_back(sk);
        (unique_keys).push_back(idx);
      }
      ++line_num;
    }
    std::sort(all_keys.begin(), all_keys.end(), base_->sort_finder);
    std::sort((unique_keys).begin(), (unique_keys).end());
    (unique_keys).erase(unique((unique_keys).begin(), (unique_keys).end()), (unique_keys).end());

    auto w = std::vector<float>();
    kv_w->Wait(kv_w->Pull(unique_keys, &w));
    auto v = std::vector<float>();
    kv_v->Wait(kv_v->Pull(unique_keys, &v));

    auto wx = std::vector<float>(line_num);
    for(int j = 0, i = 0; j < all_keys.size();){
      size_t allkeys_fid = all_keys[j].fid;
      size_t weight_fid = (unique_keys)[i];
      if(allkeys_fid == weight_fid){
        wx[all_keys[j].sid] += w[i];
        ++j;
      }
      else if(allkeys_fid > weight_fid){
        ++i;
      }
    }

    auto v_sum = std::vector<float>(end - start);
    auto v_pow_sum = std::vector<float>(end - start);
    for (size_t k = 0; k < v_dim_; ++k) {
      for (size_t j = 0, i = 0; j < all_keys.size();) {
        size_t allkeys_fid = all_keys[j].fid;
        size_t weight_fid = unique_keys[i];
        if (allkeys_fid == weight_fid) {
          size_t sid = all_keys[j].sid;
          float v_weight = v[i * v_dim_ + k];
          v_sum[sid] += v_weight;
          v_pow_sum[sid] += v_weight * v_weight;
          ++j;
        } else if (allkeys_fid > weight_fid) {
          ++i;
        }
      }
    }
    auto v_y = std::vector<float>(end - start);
    for (size_t i = 0; i < end - start; ++i) {
      v_y[i] = v_sum[i] * v_sum[i] - v_pow_sum[i];
    }

    for(int i = 0; i < wx.size(); ++i){
      float pctr = base_->sigmoid(wx[i] + v_y[i]);
      Base::auc_key ak;
      ak.label = test_data->label[start++];
      ak.pctr = pctr;
      mutex.lock();
      test_auc_vec.push_back(ak);
      md<<pctr<<"\t"<<1 - ak.label<<"\t"<<ak.label<<std::endl;
      mutex.unlock();
    }
    --calculate_pctr_thread_finish_num;
 }//calculate_pctr

  void predict(ThreadPool* pool, int rank, int block){
    char buffer[1024];
    snprintf(buffer, 1024, "%d_%d", rank, block);
    std::string filename = buffer;
    md.open("pred_" + filename + ".txt");
    if(!md.is_open()) std::cout<<"open pred file failure!"<<std::endl;

    snprintf(test_data_path, 1024, "%s-%05d", test_file_path, rank);
    xflow::LoadData test_data_loader(test_data_path, ((size_t)4)<<16);
    test_data = &(test_data_loader.m_data);
    test_auc_vec.clear();
    while(true){
      test_data_loader.load_minibatch_hash_data_fread();
      if(test_data->fea_matrix.size() <= 0) break;
      int thread_size = test_data->fea_matrix.size() / core_num;
      calculate_pctr_thread_finish_num = core_num;
      for(int i = 0; i < core_num; ++i){
        int start = i * thread_size;
        int end = (i + 1)* thread_size;
        pool->enqueue(std::bind(&FMWorker::calculate_pctr, this, start, end));
      }//end all batch
      while(calculate_pctr_thread_finish_num > 0) usleep(10);
    }//end while
    md.close();
    test_data = NULL;
    base_->calculate_auc(test_auc_vec);
  }//end predict 

  void calculate_loss(std::vector<float>& w,
                      std::vector<float>& v,
                      std::vector<Base::sample_key>& all_keys,
                      std::vector<ps::Key>& unique_keys,
                      size_t start,
                      size_t end,
                      std::vector<float>& push_w_gradient,
                      std::vector<float>& push_v_gradient) {
    auto wx = std::vector<float>(end - start);
    for(int j = 0, i = 0; j < all_keys.size();) {
      size_t allkeys_fid = all_keys[j].fid;
      size_t weight_fid = (unique_keys)[i];
      if(allkeys_fid == weight_fid) {
        wx[all_keys[j].sid] += (w)[i];
        ++j;
      } else if(allkeys_fid > weight_fid){
        ++i;
      }
    }
    // *  calculate latent v
    auto v_sum = std::vector<float>(end - start);
    auto v_pow_sum = std::vector<float>(end - start);
    for (size_t k = 0; k < v_dim_; k++) {
      for(size_t j = 0, i = 0; j < all_keys.size();) {
        size_t allkeys_fid = all_keys[j].fid;
        size_t weight_fid = unique_keys[i];
        if (allkeys_fid == weight_fid) {
          size_t sid = all_keys[j].sid;
          float v_weight = v[i * v_dim_ + k];
          v_sum[sid] += v_weight;
          v_pow_sum[sid] += v_weight * v_weight;
          ++j;
        } else if (allkeys_fid > weight_fid) {
          ++i;
        }
      }
    }
    auto v_y = std::vector<float>(end - start);
    for (size_t i = 0; i < end - start; ++i) {
      v_y[i] = v_sum[i] * v_sum[i] - v_pow_sum[i];
    }

    for(int i = 0; i < wx.size(); i++){
      float pctr = base_->sigmoid(wx[i] + v_y[i]);
      float loss = pctr - train_data->label[start++];
      wx[i] = loss;
    }

    for(int j = 0, i = 0; j < all_keys.size();){
      size_t allkeys_fid = all_keys[j].fid;
      size_t weight_fid = unique_keys[i];
      int sid = all_keys[j].sid;
      if(allkeys_fid == weight_fid){
        (push_w_gradient)[i] += wx[sid];
        push_v_gradient[i] += wx[sid] * (v_sum[sid] - unique_keys[i]);
        ++j;
      }
      else if(allkeys_fid > weight_fid){
        ++i;
      }
    }
  }

  void calculate_gradient(int start, int end){
    size_t idx = 0;
    auto all_keys = std::vector<Base::sample_key>();
    auto unique_keys = std::vector<ps::Key>();
    int line_num = 0;
    for(int row = start; row < end; ++row){
      int sample_size = train_data->fea_matrix[row].size();
      Base::sample_key sk;
      sk.sid = line_num;
      for(int j = 0; j < sample_size; ++j){
        idx = train_data->fea_matrix[row][j].fid;
        sk.fid = idx;
        all_keys.push_back(sk);
        (unique_keys).push_back(idx);
      }
      ++line_num;
    }
    std::sort(all_keys.begin(), all_keys.end(), base_->sort_finder);
    std::sort((unique_keys).begin(), (unique_keys).end());
    (unique_keys).erase(unique((unique_keys).begin(), (unique_keys).end()), (unique_keys).end());
    int keys_size = (unique_keys).size();

    auto w = std::vector<float>();
    kv_w->Wait(kv_w->Pull(unique_keys, &w));
    auto push_w_gradient = std::vector<float>(keys_size);
    auto v = std::vector<float>();
    kv_v->Wait(kv_v->Pull(unique_keys, &v));

    auto push_v_gradient = std::vector<float>(keys_size * v_dim_);
  
    calculate_loss(w, v, all_keys, unique_keys, start, end, push_w_gradient, push_v_gradient);

    for(size_t i = 0; i < (push_w_gradient).size(); ++i){
      (push_w_gradient)[i] /= 1.0 * line_num;
    }

    kv_w->Wait(kv_w->Push(unique_keys, push_w_gradient));
    --gradient_thread_finish_num;
  }

  void batch_training(ThreadPool* pool){
    std::vector<ps::Key> key(1);
    std::vector<float> val_w(1);
    std::vector<float> val_v(v_dim_);
    kv_w->Wait(kv_w->Push(key, val_w));
    kv_v->Wait(kv_v->Push(key, val_v));
    for(int epoch = 0; epoch < epochs; ++epoch){
      xflow::LoadData train_data_loader(train_data_path, block_size<<20);
      train_data = &(train_data_loader.m_data);
      int block = 0;
      while(1){
        train_data_loader.load_minibatch_hash_data_fread();
        if(train_data->fea_matrix.size() <= 0) break;
        int thread_size = train_data->fea_matrix.size() / core_num;
        gradient_thread_finish_num = core_num;
        for(int i = 0; i < core_num; ++i){
          int start = i * thread_size;
          int end = (i + 1)* thread_size;
          pool->enqueue(std::bind(&FMWorker::calculate_gradient, this, start, end));
        }
        while(gradient_thread_finish_num > 0){
          usleep(5);
        }
        ++block;
      }
      std::cout << "epoch : " << epoch << std::endl;
      train_data = NULL;
    }
  }

  void train(){
    rank = ps::MyRank();
    std::cout << "my rank is = " << rank << std::endl;
    snprintf(train_data_path, 1024, "%s-%05d", train_file_path, rank);
    batch_training(pool_);
    if (rank == 0) predict(pool_, rank, 0);
    std::cout<<"train end......"<<std::endl;
  }

 public:
  int rank;
  int core_num;
  int batch_num;
  int block_size = 2;
  int epochs = 50;

  std::atomic_llong num_batch_fly = {0};
  std::atomic_llong gradient_thread_finish_num = {0};
  std::atomic_llong calculate_pctr_thread_finish_num = {0};

  float logloss = 0.0;
  std::vector<Base::auc_key> auc_vec;
  std::vector<Base::auc_key> test_auc_vec;

  std::ofstream md;
  std::mutex mutex;
  Base* base_;
  ThreadPool* pool_;
  xflow::Data *train_data;
  xflow::Data *test_data;
  const char *train_file_path;
  const char *test_file_path;
  char train_data_path[1024];
  char test_data_path[1024];
  int v_dim_ = 10;
  ps::KVWorker<float>* kv_w;
  ps::KVWorker<float>* kv_v;
};//end class worker
}
