/**
 * @file   async_sgd.h
 * @brief  Asynchronous stochastic gradient descent to solve linear methods.
 */
#include "config.pb.h"
#include "linear.h"
#include "solver/async_sgd.h"
#include "base/localizer.h"
#include "loss.h"
#include "penalty.h"

namespace dmlc {
namespace linear {

template <typename T> using Blob = ps::Blob<T>;

/*********************************************************************
 * \brief the base handle class
 *********************************************************************/
struct ISGDHandle {
 public:
  inline void Start(bool push, int timestamp, int cmd, void* msg) { }
  // report
  inline void Finish() {
    Progress prog;
    if (reporter) reporter(prog);
  }

  inline void Report(Real cur_w, Real old_w) {

  }

  L1L2<Real> penalty;

  // learning rate
  Real alpha = 0.1, beta = 1;

  std::function<void(const Progress& prog)> reporter;
};

/*********************************************************************
 * \brief Standard SGD
 * use alpha / ( beta + sqrt(t)) as the learning rate
 *********************************************************************/
struct SGDHandle : public ISGDHandle {
 public:
  inline void Start(bool push, int timestamp, int cmd, void* msg) {
    if (push) {
      eta = (this->beta + sqrt((Real)t)) / this->alpha;
      t += 1;
    }
  }
  inline void Push(FeaID key, Blob<const Real> grad, Real& w) {
    Real old_w = w;
    w = penalty.Solve(eta * w - grad[0], eta);
    Report(w, old_w);
  }

  inline void Pull(FeaID key, const Real& w, Blob<Real>& send) {
    send[0] = w;
  }
  // iteration count
  int t = 1;
  Real eta = 0;
};


/*********************************************************************
 * \brief AdaGrad SGD handle.
 * use alpha / ( beta + sqrt(sum_t grad_t^2)) as the learning rate
 *
 * sq_cum_grad: sqrt(sum_t grad_t^2)
 *********************************************************************/

struct AdaGradEntry { Real w = 0; Real sq_cum_grad = 0; };

struct AdaGradHandle : public ISGDHandle {
  inline void Init(FeaID key,  AdaGradEntry& val) { }

  inline void Push(FeaID key, Blob<const Real> grad, AdaGradEntry& val) {
    // update cum grad
    Real g = grad[0];
    Real sqrt_n = val.sq_cum_grad;
    val.sq_cum_grad = sqrt(sqrt_n * sqrt_n + g * g);

    // update w
    Real eta = (val.sq_cum_grad + beta) / alpha;
    Real old_w = val.w;
    val.w = penalty.Solve(eta * old_w - g, eta);

    Report(val.w, old_w);
  }

  inline void Pull(FeaID key, const AdaGradEntry& val, Blob<Real>& send) {
    send[0] = val.w;
  }
};

/*********************************************************************
 * \brief FTRL updater, use a smoothed weight for better spasity
 *
 * w : weight
 * z : the smoothed version of - eta * w + grad
 * sq_cum_grad: sqrt(sum_t grad_t^2)
 *********************************************************************/

struct FTRLEntry { Real w = 0; Real z = 0; Real sq_cum_grad = 0; };

struct FTRLHandle : public ISGDHandle {
 public:
  inline void Init(FeaID key,  FTRLEntry& val) { }

  inline void Push(FeaID key, Blob<const Real> grad, FTRLEntry& val) {
    // update cum grad
    Real g = grad[0];
    Real sqrt_n = val.sq_cum_grad;
    val.sq_cum_grad = sqrt(sqrt_n * sqrt_n + g * g);

    // update z
    Real old_w = val.w;
    Real sigma = (val.sq_cum_grad - sqrt_n) / alpha;
    val.z += g - sigma * old_w;

    // update w
    val.w = penalty.Solve(-val.z, (beta + val.sq_cum_grad) / alpha);

    Report(val.w, old_w);
  }

  inline void Pull(FeaID key, const FTRLEntry& val, Blob<Real>& send) {
    send[0] = val.w;
  }
};


class AsgdServer : public solver::AsyncSGDServer {
 public:
  AsgdServer(const Config& conf) : conf_(conf) {
    auto algo = conf_.algo();
    if (algo == Config::SGD) {
      CreateServer<Real, SGDHandle>();
    } else if (algo == Config::ADAGRAD) {
      CreateServer<AdaGradEntry, AdaGradHandle>();
    } else if (algo == Config::FTRL) {
      CreateServer<FTRLEntry, FTRLHandle>();
    } else {
      LOG(FATAL) << "unknown algo: " << algo;
    }
  }
  virtual ~AsgdServer() { }
 protected:
  template <typename Entry, typename Handle>
  void CreateServer() {
    Handle h;
    h.penalty.set_lambda1(conf_.lambda_l1());
    h.penalty.set_lambda2(conf_.lambda_l2());
    if (conf_.has_lr_eta()) h.alpha = conf_.lr_eta();
    if (conf_.has_lr_beta()) h.beta = conf_.lr_beta();
    ps::OnlineServer<Entry, Real, Handle> s(h);
    server_ = s.server();
  }

  Config conf_;
  virtual void SaveModel() { }
  ps::KVStore* server_;
};

class AsgdWorker : public solver::AsyncSGDWorker {
 public:
  AsgdWorker(const Config& conf) : conf_(conf) {
    minibatch_size_ = conf.minibatch();
    max_delay_      = conf.max_delay();
    nt_             = conf.num_threads();
    if (conf.use_worker_local_data()) {
      train_data_        = conf.train_data();
      val_data_          = conf.val_data();
      worker_local_data_ = true;
    }
  }
  virtual ~AsgdWorker() { }

 protected:
  virtual void ProcessMinibatch(const Minibatch& mb, int data_pass, bool train) {
    // find the unique feature ids in this minibatch
    auto data = new dmlc::data::RowBlockContainer<unsigned>();
    auto feaid = std::make_shared<std::vector<FeaID>>();
    Localizer<FeaID> lc;
    lc.Localize(mb, data, feaid.get());

    // pull the weight from the servers
    auto val = new std::vector<Real>();
    ps::SyncOpts pull_w_opt;

    // this callback will be called when the weight has been actually pulled
    // back
    pull_w_opt.callback = [this, data, feaid, val, train]() {
      // eval the objective, and report progress to the scheduler
      auto loss = CreateLoss(conf_.loss());
      loss->Init(data->GetBlock(), *val, nt_);
      Progress prog; loss->Evaluate(&prog);
      Report(&prog);

      if (train) {
        // calculate and push the gradients
        loss->CalcGrad(val);

        ps::SyncOpts push_grad_opt;
        // filters to reduce network traffic
        SetFilters(train, &push_grad_opt);
        // this callback will be called when the gradients have been actually
        // pushed
        push_grad_opt.callback = [this]() { FinishMinibatch(); };
        server_.ZPush(
            feaid, std::shared_ptr<std::vector<Real>>(val), push_grad_opt);
      } else {
        FinishMinibatch();
        delete val;
      }
      delete loss;
      delete data;
    };
    server_.ZPull(feaid, val, pull_w_opt);
  }
 private:
  void SetFilters(bool push, ps::SyncOpts* opts) {
    if (conf_.fixed_bytes() > 0) {
      opts->AddFilter(ps::Filter::FIXING_FLOAT)->set_num_bytes(
          conf_.fixed_bytes());
    }
    if (conf_.key_cache()) {
      opts->AddFilter(ps::Filter::KEY_CACHING)->set_clear_cache(push);
    }
    if (conf_.msg_compression()) {
      opts->AddFilter(ps::Filter::COMPRESSING);
    }
  }
  Config conf_;
  ps::KVWorker<Real> server_;
  int nt_ = 2;
};


class AsgdScheduler : public solver::AsyncSGDScheduler<Progress> {
 public:
  AsgdScheduler(const Config& conf) {
    if (conf.use_worker_local_data()) {
      worker_local_data_ = true;
    } else {
      train_data_        = conf.train_data();
      val_data_          = conf.val_data();
    }
    data_format_         = conf.data_format();
    num_part_per_file_   = conf.num_parts_per_file();
    max_data_pass_       = conf.max_data_pass();
    disp_itv_            = conf.disp_itv();
  }
  virtual ~AsgdScheduler() { }
};

}  // namespace linear
}  // namespace dmlc
