/**
 * @file   async_sgd.h
 * @brief  Asynchronous stochastic gradient descent to solve linear methods.
 */
#include "proto/config.pb.h"
#include "proto/sys.pb.h"
#include "dmlc/timer.h"
#include "base/minibatch_iter.h"
#include "base/arg_parser.h"
#include "base/localizer.h"
#include "base/loss.h"
#include "sgd/sgd_server_handle.h"
#include "base/dist_monitor.h"
#include "base/workload_pool.h"

#include "base/utils.h"
#include "ps.h"
#include "ps/app.h"

namespace dmlc {
namespace linear {
#define LL LOG(ERROR)

using FeaID = ps::Key;
using Real = float;

// commands
static const int kProcess = 1;
static const int kSaveModel = 2;


/***************************************
 * \brief The scheduler node
 **************************************/
class AsyncSGDScheduler : public ps::App {
 public:
  AsyncSGDScheduler(const Config& conf) : conf_(conf) { }
  virtual ~AsyncSGDScheduler() { }

  virtual void ProcessResponse(ps::Message* response) {
    if (response->task.cmd() == kProcess) {
      auto id = response->sender;
      if (!response->task.msg().empty()) {
        Progress p;
        p.Parse(response->task.msg());
        prog_.Merge(p);
      }
      pool_.Finish(id);
      Workload wl; pool_.Get(id, &wl);
      if (wl.file_size() > 0) SendWorkload(id, wl);
    }
  }

  virtual void Run() {
    // wait nodes are ready
    ps::App::Run();

    CHECK(conf_.has_train_data());
    double t = GetTime();
    size_t num_ex = 0;
    for (int i = 0; i < conf_.max_data_pass(); ++i) {
      printf("training #iter = %d\n", i);
      // train
      pool_.Add(conf_.train_data(), conf_.num_parts_per_file(), 0, Workload::TRAIN);
      Workload wl; SendWorkload(ps::kWorkerGroup, wl);

      sleep(1);
      while (!pool_.IsFinished()) {
        sleep((int) conf_.disp_itv());
        Progress prog; monitor_.Get(0, &prog);
        monitor_.Clear(0);
        if (prog.Empty()) continue;
        num_ex += prog.num_ex();
        printf("%7.1lf sec, #train %.3g, %s\n",
               GetTime() - t, (double)num_ex, prog.PrintStr().c_str());
      }

      // val
      if (!conf_.has_val_data()) continue;
      printf("validation #iter = %d\n", i);
      pool_.Add(conf_.val_data(), conf_.num_parts_per_file(), 0, Workload::VAL);
      SendWorkload(ps::kWorkerGroup, wl);

      while (!pool_.IsFinished()) { sleep(1); }

      printf("%7.1lf sec, #val %.3g, %s\n",
             GetTime() - t, (double)prog_.num_ex(), prog_.PrintStr().c_str());
      prog_.Clear();
    }

    printf("saving model");
    ps::Task task; task.set_cmd(kSaveModel);
    Wait(Submit(task, ps::kServerGroup));
  }
 private:
  void SendWorkload(const std::string id, const Workload& wl) {
    std::string wl_str; wl.SerializeToString(&wl_str);
    ps::Task task; task.set_msg(wl_str);
    task.set_cmd(kProcess);
    Submit(task, id);
  }

  Config conf_;
  WorkloadPool pool_;
  bool done_ = false;
  Progress prog_;
  ps::MonitorMaster<Progress> monitor_;
};

/***************************************
 * \brief A server node
 **************************************/
class AsyncSGDServer : public ps::App {
 public:
  AsyncSGDServer(const Config& conf) : conf_(conf), monitor_(conf_.disp_itv())  {
    Init();
  }
  virtual ~AsyncSGDServer() { }

  virtual void ProcessRequest(ps::Message* request) {
    if (request->task.cmd() == kSaveModel) {
      // TODO
    }
  }

 private:
  void Init() {
    auto algo = conf_.algo();
    if (algo == Config::FTRL) {
      ps::KVServer<Real, FTRLHandle<FeaID, Real>, 3> ftrl;
      ftrl.set_sync_val_len(1);
      auto& updt = ftrl.handle();
      if (conf_.has_lr_eta()) updt.alpha = conf_.lr_eta();
      if (conf_.has_lr_beta()) updt.beta = conf_.lr_beta();
      if (conf_.lambda_size() > 0) updt.lambda1 = conf_.lambda(0);
      if (conf_.lambda_size() > 1) updt.lambda2 = conf_.lambda(1);
      updt.tracker = &monitor_;
      ftrl.Run();
    } else {
      LOG(FATAL) << "unknown algo: " << algo;
    }
  }
  Config conf_;
  DistModelMonitor monitor_;
};

/***************************************
 * \brief A worker node
 **************************************/
class AsyncSGDWorker : public ps::App {
 public:
  AsyncSGDWorker(const Config& conf) : conf_(conf), reporter_(conf_.disp_itv()) { }
  virtual ~AsyncSGDWorker() { }

  virtual void ProcessRequest(ps::Message* request) {
    int cmd = request->task.cmd();
    if (cmd == kProcess) {
      Workload wl; CHECK(wl.ParseFromString(request->task.msg()));
      if (wl.file_size() < 1) return;
      Process(wl.file(0), wl.type());
      if (wl.type() != Workload::TRAIN) {
        // return the progress
        std::string prog_str; monitor_.prog.Serialize(&prog_str);
        ps::Task res; res.set_msg(prog_str);
        Reply(request, res);
      }
    }
  }

 private:
  void Process(const File& file, Workload::Type type) {
    // use a large minibatch size and max_delay for val or test tasks
    int mb_size = type == Workload::TRAIN ? conf_.minibatch() :
                  std::max(conf_.minibatch()*10, 100000);
    int max_delay = type == Workload::TRAIN ? conf_.max_delay() : 100000;
    num_mb_fly_ = num_mb_done_ = 0;

    LOG(INFO) << ps::MyNodeID() << ": start to process " << file.ShortDebugString();
    dmlc::data::MinibatchIter<FeaID> reader(
        file.file().c_str(), file.k(), file.n(),
        conf_.data_format().c_str(), mb_size);
    reader.BeforeFirst();
    while (reader.Next()) {

      using std::vector;
      using std::shared_ptr;
      using Minibatch = dmlc::data::RowBlockContainer<unsigned>;

      // localize the minibatch
      auto global = reader.Value();
      Minibatch* local = new Minibatch();
      shared_ptr<vector<FeaID> > feaid(new vector<FeaID>());
      Localizer<FeaID> lc; lc.Localize(global, local, feaid.get());

      // pull the weight
      vector<Real>* buf = new vector<Real>(feaid.get()->size());
      ps::SyncOpts opts;
      opts.callback = [this, local, feaid, buf, type]() {
        // eval
        auto loss = CreateLoss<real_t>(conf_.loss());
        loss->Init(local->GetBlock(), *buf);
        monitor_.Update(local->label.size(), loss);

        if (type == Workload::TRAIN) {
          // continous reporting
          reporter_.Report(0, &monitor_.prog);

          // calc and push grad
          loss->CalcGrad(buf);
          ps::SyncOpts opts;
          opts.callback = [this]() {
            FinishMinibatch();
          };
          server_.ZPush(feaid, shared_ptr<vector<Real>>(buf), opts);
        } else {
          FinishMinibatch();
          delete buf;
        }
        delete local;
        delete loss;
      };
      server_.ZPull(feaid, buf, opts);

      // wait for data consistency
      std::unique_lock<std::mutex> lk(mb_mu_);
      ++ num_mb_fly_;
      mb_cond_.wait(lk, [this, max_delay] {return max_delay >= num_mb_fly_;});
      // LL << num_mb_fly_;
    }

    // wait untill all are done
    std::unique_lock<std::mutex> lk(mb_mu_);
    mb_cond_.wait(lk, [this] {return num_mb_fly_ <= 0;});
    LOG(INFO) << ps::MyNodeID() << ": finished";
  }

  void FinishMinibatch() {
    // wake the main thread
    mb_mu_.lock(); -- num_mb_fly_; ++ num_mb_done_; mb_mu_.unlock();
    mb_cond_.notify_one();
  }
  Config conf_;
  ps::KVWorker<Real> server_;
  WorkerMonitor monitor_;
  TimeReporter reporter_;

  int num_mb_fly_;
  int num_mb_done_;
  std::mutex mb_mu_;
  std::condition_variable mb_cond_;

};


}  // namespace linear
}  // namespace dmlc
