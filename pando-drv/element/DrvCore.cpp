// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPIGlobal.hpp>
#include <DrvAPIAllocator.hpp>
#include <DrvAPIAddress.hpp>
#include <DrvAPIAddressMap.hpp>
#include "DrvCore.hpp"
#include "DrvSimpleMemory.hpp"
#include "DrvSelfLinkMemory.hpp"
#include "DrvStdMemory.hpp"
#include "DrvNopEvent.hpp"
#include <cstdio>
#include <dlfcn.h>
#include <cstring>
#include <sstream>
#include <iomanip>

using namespace SST;
using namespace Drv;

//////////////////////////
// init and finish code //
//////////////////////////
/**
 * configure the output logging
 */
void DrvCore::configureOutput(SST::Params &params) {
  int verbose_level = params.find<int>("verbose");
  uint32_t verbose_mask = 0;
  if (params.find<bool>("debug_init"))
    verbose_mask |= DEBUG_INIT;
  if (params.find<bool>("debug_clock"))
    verbose_mask |= DEBUG_CLK;
  if (params.find<bool>("debug_requests"))
    verbose_mask |= DEBUG_REQ;
  if (params.find<bool>("debug_responses"))
    verbose_mask |= DEBUG_RSP;
  if (params.find<bool>("debug_mmio"))
    verbose_mask |= DEBUG_MMIO;
  std::stringstream ss;
  ss << "[DrvCore "
     << "{PXN=" << std::setw(2) << pxn_
     << ",POD=" << std::setw(2) <<  pod_
     << ",CORE=" << std::setw(2) << id_
     << "} @t: @f:@l: @p] ";
  output_ = std::make_unique<SST::Output>(ss.str(), verbose_level, verbose_mask, Output::STDOUT);
  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configured output logging\n");
}

void DrvCore::configureTrace(SST::Params &params) {
    uint32_t trace_mask = 0;
    if (params.find<bool>("trace_remote_pxn"))
        trace_mask |= TRACE_REMOTE_PXN_MEMORY;
    if (params.find<bool>("trace_remote_pxn_load"))
        trace_mask |= TRACE_REMOTE_PXN_LOAD;
    if (params.find<bool>("trace_remote_pxn_store"))
        trace_mask |= TRACE_REMOTE_PXN_STORE;
    if (params.find<bool>("trace_remote_pxn_atomic"))
        trace_mask |= TRACE_REMOTE_PXN_ATOMIC;
    trace_ = std::make_unique<SST::Output>("@t:", 0, trace_mask, Output::FILE);
}

/**
 * configure the executable */
void DrvCore::configureExecutable(SST::Params &params) {
  std::string executable = params.find<std::string>("executable");
  argv_.push_back(strdup(executable.c_str()));
  if (executable.empty()) {
    output_->fatal(CALL_INFO, -1, "executable not specified\n");
  }

  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring executable: %s\n", executable.c_str());
  executable_ = dlopen(executable.c_str(), RTLD_LAZY|RTLD_LOCAL);
  if (!executable_) {
    output_->fatal(CALL_INFO, -1, "unable to open executable: %s\n", dlerror());
  }

  main_ = (drv_api_main_t)dlsym(executable_, "__drv_api_main");
  if (!main_) {
    output_->fatal(CALL_INFO, -1, "unable to find __drv_api_main in executable: %s\n", dlerror());
  }

  get_thread_context_ = (drv_api_get_thread_context_t)dlsym(executable_, "DrvAPIGetCurrentContext");
  if (!get_thread_context_) {
    output_->fatal(CALL_INFO, -1, "unable to find DrvAPIGetCurrentContext in executable: %s\n", dlerror());
  }

  set_thread_context_ = (drv_api_set_thread_context_t)dlsym(executable_, "DrvAPISetCurrentContext");
  if (!set_thread_context_) {
    output_->fatal(CALL_INFO, -1, "unable to find DrvAPISetCurrentContext in executable: %s\n", dlerror());
  }

  get_sys_config_app_ = (DrvAPIGetSysConfig_t)dlsym(executable_, "DrvAPIGetSysConfig");
  if (!get_sys_config_app_) {
      output_->fatal(CALL_INFO, -1, "unable to find DrvAPIGetSysConfig in executable: %s\n", dlerror());
  }

  set_sys_config_app_ = (DrvAPISetSysConfig_t)dlsym(executable_, "DrvAPISetSysConfig");
  if (!set_sys_config_app_) {
      output_->fatal(CALL_INFO, -1, "unable to find DrvAPISetSysConfig in executable: %s\n", dlerror());
  }

  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configured executable\n");
}

/**
 * close the executable
 */
void DrvCore::closeExecutable() {
  if (dlclose(executable_)) {
    output_->fatal(CALL_INFO, -1, "unable to close executable: %s\n", dlerror());
  }
  executable_ = nullptr;
}

/**
 * configure the clock and register the handler
 */
void DrvCore::configureClock(SST::Params &params) {
  clock_handler_ = new Clock::Handler<DrvCore>(this, &DrvCore::clockTick);
  clocktc_ = registerClock(params.find<std::string>("clock", "125MHz"), clock_handler_);
  max_idle_cycles_ = params.find<uint64_t>("max_idle", 1000000);
  core_on_ = true;
}

/**
 * configure one thread
 */
void DrvCore::configureThread(int thread, int threads) {
  output_->verbose(CALL_INFO, 2, DEBUG_INIT, "configuring thread (%2d/%2d)\n", thread, threads);
  threads_.emplace_back();
  DrvAPI::DrvAPIThread& api_thread = threads_.back().getAPIThread();
  api_thread.setMain(main_);
  api_thread.setArgs(argv_.size(), argv_.data());
  api_thread.setId(thread);
  api_thread.setCoreId(id_);
  api_thread.setCoreThreads(threads);
  api_thread.setPodId(pod_);
  api_thread.setPxnId(pxn_);
  api_thread.setStackInL1SP(stack_in_l1sp_);
  api_thread.setSystem(system_callbacks_);
}

/**
 * configure threads on the core
 */
void DrvCore::configureThreads(SST::Params &params) {
  int threads = params.find<int>("threads", 1);
  stack_in_l1sp_ = params.find<bool>("stack_in_l1sp", false);
  output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring %d threads\n", threads);
  for (int thread = 0; thread < threads; thread++)
    configureThread(thread, threads);
  done_ = threads;
  last_thread_ = threads - 1;
}

void DrvCore::startThreads() {
    for (auto& thread : threads_) {
        thread.getAPIThread().start();
    }
}

/**
 * configure the memory
 */
void DrvCore::configureMemory(SST::Params &params) {
    memory_ = loadUserSubComponent<DrvMemory>("memory", ComponentInfo::SHARE_NONE, this);
    if (!memory_) {
        output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring simple memory\n");
        SST::Params mem_params = params.get_scoped_params("memory");
        memory_ = loadAnonymousSubComponent<DrvMemory>
            ("Drv.DrvSimpleMemory", "memory", 0, ComponentInfo::SHARE_NONE, mem_params, this);
        if (!memory_) {
            output_->fatal(CALL_INFO, -1, "unable to load memory subcomponent\n");
        }
    }
    DrvAPI::DrvAPIAddress dram_base_default
        = DrvAPI::DrvAPIVAddress::MainMemBase(pxn_).encode();
    DrvAPI::DrvAPISection::GetSection(DrvAPI::DrvAPIMemoryDRAM)
        .setBase(dram_base_default, pxn_, pod_, id_);

    // default l2 statics to base of local l2 scratchpad
    DrvAPI::DrvAPIAddress l2sp_base_default = DrvAPI::DrvAPIVAddress::MyL2Base().encode();
    uint64_t l2sp_base = params.find<uint64_t>("l2sp_base", l2sp_base_default);
    DrvAPI::DrvAPISection::GetSection(DrvAPI::DrvAPIMemoryL2SP)
        .setBase(l2sp_base, pxn_, pod_, id_);


    // default l1 statics to base of local l1 scratchpad
    DrvAPI::DrvAPIAddress l1sp_base_default = DrvAPI::DrvAPIVAddress::MyL1Base().encode();
    uint64_t l1sp_base = params.find<uint64_t>("l1sp_base", l1sp_base_default);
    DrvAPI::DrvAPISection::GetSection(DrvAPI::DrvAPIMemoryL1SP)
        .setBase(l1sp_base, pxn_, pod_, id_);
}

/*
 * configure the other links
 */
void DrvCore::configureOtherLinks(SST::Params &params) {
    loopback_ = configureSelfLink("loopback", new Event::Handler<DrvCore>(this, &DrvCore::handleLoopback));
    loopback_->addSendLatency(1, "ns");
}

/**
 * configure phase statistics
 */
void DrvCore::configurePhaseStatistics() {
    output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring phase statistics\n");

    for (int phase = 0; phase < phase_max_; phase++) {
      std::vector<ThreadStat> phase_comp_thread_stats;
      phase_comp_thread_stats.resize(num_threads_);
      for (int thread = 0; thread < num_threads_; thread++) {
          ThreadStat *thread_stat = &phase_comp_thread_stats[thread];
          std::string subid = "phase_" + std::to_string(phase) + "_thread_" + std::to_string(thread);
          thread_stat->load_l1sp = registerStatistic<uint64_t>("phase_comp_load_l1sp", subid);
          thread_stat->load_l2sp = registerStatistic<uint64_t>("phase_comp_load_l2sp", subid);
          thread_stat->load_dram = registerStatistic<uint64_t>("phase_comp_load_dram", subid);
          thread_stat->load_remote_pxn = registerStatistic<uint64_t>("phase_comp_load_remote_pxn", subid);
          thread_stat->store_l1sp = registerStatistic<uint64_t>("phase_comp_store_l1sp", subid);
          thread_stat->store_l2sp = registerStatistic<uint64_t>("phase_comp_store_l2sp", subid);
          thread_stat->store_dram = registerStatistic<uint64_t>("phase_comp_store_dram", subid);
          thread_stat->store_remote_pxn = registerStatistic<uint64_t>("phase_comp_store_remote_pxn", subid);
          thread_stat->atomic_l1sp = registerStatistic<uint64_t>("phase_comp_atomic_l1sp", subid);
          thread_stat->atomic_l2sp = registerStatistic<uint64_t>("phase_comp_atomic_l2sp", subid);
          thread_stat->atomic_dram = registerStatistic<uint64_t>("phase_comp_atomic_dram", subid);
          thread_stat->atomic_remote_pxn = registerStatistic<uint64_t>("phase_comp_atomic_remote_pxn", subid);
          thread_stat->stall_cycles_when_ready = registerStatistic<uint64_t>("phase_comp_stall_cycles_when_ready", subid);
          thread_stat->tag_cycles = registerStatistic<uint64_t>("phase_comp_tag_cycles", subid);

          thread_stat->load_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->load_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->load_dram->setFlagResetCountOnOutput(true);
          thread_stat->load_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->store_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->store_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->store_dram->setFlagResetCountOnOutput(true);
          thread_stat->store_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->atomic_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->atomic_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->atomic_dram->setFlagResetCountOnOutput(true);
          thread_stat->atomic_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->stall_cycles_when_ready->setFlagResetCountOnOutput(true);
          thread_stat->tag_cycles->setFlagResetCountOnOutput(true);

          thread_stat->load_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->load_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->load_dram->setFlagClearDataOnOutput(true);
          thread_stat->load_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->store_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->store_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->store_dram->setFlagClearDataOnOutput(true);
          thread_stat->store_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->atomic_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->atomic_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->atomic_dram->setFlagClearDataOnOutput(true);
          thread_stat->atomic_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->stall_cycles_when_ready->setFlagClearDataOnOutput(true);
          thread_stat->tag_cycles->setFlagClearDataOnOutput(true);
      }
      per_phase_comp_thread_stats_.push_back(phase_comp_thread_stats);

      std::vector<ThreadStat> phase_comm_thread_stats;
      phase_comm_thread_stats.resize(num_threads_);
      for (int thread = 0; thread < num_threads_; thread++) {
          ThreadStat *thread_stat = &phase_comm_thread_stats[thread];
          std::string subid = "phase_" + std::to_string(phase) + "_thread_" + std::to_string(thread);
          thread_stat->load_l1sp = registerStatistic<uint64_t>("phase_comm_load_l1sp", subid);
          thread_stat->load_l2sp = registerStatistic<uint64_t>("phase_comm_load_l2sp", subid);
          thread_stat->load_dram = registerStatistic<uint64_t>("phase_comm_load_dram", subid);
          thread_stat->load_remote_pxn = registerStatistic<uint64_t>("phase_comm_load_remote_pxn", subid);
          thread_stat->store_l1sp = registerStatistic<uint64_t>("phase_comm_store_l1sp", subid);
          thread_stat->store_l2sp = registerStatistic<uint64_t>("phase_comm_store_l2sp", subid);
          thread_stat->store_dram = registerStatistic<uint64_t>("phase_comm_store_dram", subid);
          thread_stat->store_remote_pxn = registerStatistic<uint64_t>("phase_comm_store_remote_pxn", subid);
          thread_stat->atomic_l1sp = registerStatistic<uint64_t>("phase_comm_atomic_l1sp", subid);
          thread_stat->atomic_l2sp = registerStatistic<uint64_t>("phase_comm_atomic_l2sp", subid);
          thread_stat->atomic_dram = registerStatistic<uint64_t>("phase_comm_atomic_dram", subid);
          thread_stat->atomic_remote_pxn = registerStatistic<uint64_t>("phase_comm_atomic_remote_pxn", subid);
          thread_stat->stall_cycles_when_ready = registerStatistic<uint64_t>("phase_comm_stall_cycles_when_ready", subid);
          thread_stat->tag_cycles = registerStatistic<uint64_t>("phase_comm_tag_cycles", subid);

          thread_stat->load_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->load_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->load_dram->setFlagResetCountOnOutput(true);
          thread_stat->load_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->store_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->store_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->store_dram->setFlagResetCountOnOutput(true);
          thread_stat->store_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->atomic_l1sp->setFlagResetCountOnOutput(true);
          thread_stat->atomic_l2sp->setFlagResetCountOnOutput(true);
          thread_stat->atomic_dram->setFlagResetCountOnOutput(true);
          thread_stat->atomic_remote_pxn->setFlagResetCountOnOutput(true);
          thread_stat->stall_cycles_when_ready->setFlagResetCountOnOutput(true);
          thread_stat->tag_cycles->setFlagResetCountOnOutput(true);

          thread_stat->load_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->load_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->load_dram->setFlagClearDataOnOutput(true);
          thread_stat->load_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->store_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->store_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->store_dram->setFlagClearDataOnOutput(true);
          thread_stat->store_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->atomic_l1sp->setFlagClearDataOnOutput(true);
          thread_stat->atomic_l2sp->setFlagClearDataOnOutput(true);
          thread_stat->atomic_dram->setFlagClearDataOnOutput(true);
          thread_stat->atomic_remote_pxn->setFlagClearDataOnOutput(true);
          thread_stat->stall_cycles_when_ready->setFlagClearDataOnOutput(true);
          thread_stat->tag_cycles->setFlagClearDataOnOutput(true);
      }
      per_phase_comm_thread_stats_.push_back(phase_comm_thread_stats);

      Statistic<uint64_t> *phase_busy_cycles;
      Statistic<uint64_t> *phase_stall_cycles;
      std::string subid = "phase_" + std::to_string(phase);
      phase_busy_cycles = registerStatistic<uint64_t>("phase_busy_cycles", subid);
      phase_stall_cycles = registerStatistic<uint64_t>("phase_stall_cycles", subid);

      phase_busy_cycles->setFlagResetCountOnOutput(true);
      phase_stall_cycles->setFlagResetCountOnOutput(true);

      phase_busy_cycles->setFlagClearDataOnOutput(true);
      phase_stall_cycles->setFlagClearDataOnOutput(true);

      per_phase_busy_cycles_.push_back(phase_busy_cycles);
      per_phase_stall_cycles_.push_back(phase_stall_cycles);
    }
}

/**
 * configure statistics
 */
void DrvCore::configureStatistics() {
    output_->verbose(CALL_INFO, 1, DEBUG_INIT, "configuring statistics\n");
    uint32_t stats_level = getStatisticLoadLevel();
    tag_.init("", stats_level, 0, Output::FILE, "tags.csv");
    tag_.verbose(CALL_INFO, 1, 0, "SimTime,TagName\n");

    total_thread_stats_.resize(num_threads_);
    for (int thread = 0; thread < num_threads_; thread++) {
        ThreadStat *total_stat = &total_thread_stats_[thread];
        std::string subid = "thread_" + std::to_string(thread);
        total_stat->load_l1sp = registerStatistic<uint64_t>("total_load_l1sp", subid);
        total_stat->load_l2sp = registerStatistic<uint64_t>("total_load_l2sp", subid);
        total_stat->load_dram = registerStatistic<uint64_t>("total_load_dram", subid);
        total_stat->load_remote_pxn = registerStatistic<uint64_t>("total_load_remote_pxn", subid);
        total_stat->store_l1sp = registerStatistic<uint64_t>("total_store_l1sp", subid);
        total_stat->store_l2sp = registerStatistic<uint64_t>("total_store_l2sp", subid);
        total_stat->store_dram = registerStatistic<uint64_t>("total_store_dram", subid);
        total_stat->store_remote_pxn = registerStatistic<uint64_t>("total_store_remote_pxn", subid);
        total_stat->atomic_l1sp = registerStatistic<uint64_t>("total_atomic_l1sp", subid);
        total_stat->atomic_l2sp = registerStatistic<uint64_t>("total_atomic_l2sp", subid);
        total_stat->atomic_dram = registerStatistic<uint64_t>("total_atomic_dram", subid);
        total_stat->atomic_remote_pxn = registerStatistic<uint64_t>("total_atomic_remote_pxn", subid);
        total_stat->stall_cycles_when_ready = registerStatistic<uint64_t>("total_stall_cycles_when_ready", subid);
        total_stat->tag_cycles = registerStatistic<uint64_t>("total_tag_cycles", subid);
    }
    total_busy_cycles_ = registerStatistic<uint64_t>("total_busy_cycles");
    total_stall_cycles_ = registerStatistic<uint64_t>("total_stall_cycles");

    DrvCore::configurePhaseStatistics();
}

/**
 * configure sysconfig
 * @param[in] params Parameters to this component.
 */
void DrvCore::configureSysConfig(SST::Params &params) {
    sys_config_.init(params);
    DrvAPI::DrvAPISysConfig cfg = sys_config_.config();
    output_->verbose(CALL_INFO, 1, DEBUG_INIT,
                     "configured sysconfig: "
                     "num_pxn = %" PRId64 ", "
                     "pxn_pods = %" PRId64 ", "
                     "pod_cores = %" PRId64 ", "
                     "core_threads = %d"
                     "\n"
                     ,cfg.numPXN()
                     ,cfg.numPXNPods()
                     ,cfg.numPodCores()
                     ,numThreads()
                     );
}

void DrvCore::parseArgv(SST::Params &params) {
    std::string argv_str = params.find<std::string>("argv", "");
    std::stringstream ss(argv_str);
    while (!ss.eof()) {
        std::string arg;
        ss >> arg;
        if (!arg.empty()) {
            argv_.push_back(strdup(arg.c_str()));
        }
    }
}

DrvCore::DrvCore(SST::ComponentId_t id, SST::Params& params)
  : SST::Component(id)
  , executable_(nullptr)
  , loopback_(nullptr)
  , idle_cycles_(0)
  , core_on_(false)
  , stage_(DrvAPI::stage_t::STAGE_OTHER)
  , phase_(0)
  , stat_dump_cnt_(0)
  , system_callbacks_(std::make_shared<DrvSystem>(*this)) {
  id_ = params.find<int>("id", 0);
  pod_ = params.find<int>("pod", 0);
  pxn_ = params.find<int>("pxn", 0);
  num_threads_ = params.find<int>("threads", 1);
  phase_max_ = params.find<int>("phase_max", 1);
  registerAsPrimaryComponent();
  primaryComponentDoNotEndSim();
  configureOutput(params);
  configureTrace(params);
  configureSysConfig(params);
  configureClock(params);
  configureMemory(params);
  configureOtherLinks(params);
  configureExecutable(params);
  configureStatistics();
  parseArgv(params);
  configureThreads(params);
  setSysConfigApp();
}

DrvCore::~DrvCore() {
    // the last thing we should is close the executable
    // this keeps the vtable entries valid for dynamic classes
    // created in the user code
    closeExecutable();
    // clean up the argv
    for (auto arg : argv_) {
        free(arg);
    }
    delete memory_;
}

///////////////////////////
// SST simulation phases //
///////////////////////////
/**
 * initialize the component
 */
void DrvCore::init(unsigned int phase) {
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->init(phase);
  }
}

/**
 * finish the component
 */
void DrvCore::setup() {
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->setup();
  }
  startThreads();
}

/**
 * finish the component
 */
void DrvCore::finish() {
  threads_.clear();
  auto stdmem = dynamic_cast<DrvStdMemory*>(memory_);
  if (stdmem) {
    stdmem->finish();
  }
}

/////////////////////
// clock tick code //
/////////////////////
static constexpr int NO_THREAD_READY = -1;

int DrvCore::selectReadyThread() {
  // select a ready thread to execute

  bool selected = false;
  int return_thread_id = NO_THREAD_READY;
  for (int t = 0; t < numThreads(); t++) {
    int thread_id = (last_thread_ + t + 1) % numThreads();
    DrvThread *thread = getThread(thread_id);
    auto & state = thread->getAPIThread().getState();

    if (state->canResume()) {
      if (!selected) {
        output_->verbose(CALL_INFO, 2, DEBUG_CLK, "thread %d is ready\n", thread_id);
        selected = true;
        return_thread_id =  thread_id;
      } else {
        if (stage_ == DrvAPI::stage_t::STAGE_EXEC_COMP) {
          ThreadStat *total_stats = &total_thread_stats_[t];
          ThreadStat *phase_stats = &(per_phase_comp_thread_stats_[phase_][t]);
          total_stats->stall_cycles_when_ready->addData(1);
          phase_stats->stall_cycles_when_ready->addData(1);
        }
        else if (stage_ == DrvAPI::stage_t::STAGE_EXEC_COMM) {
          ThreadStat *total_stats = &total_thread_stats_[t];
          ThreadStat *phase_stats = &(per_phase_comm_thread_stats_[phase_][t]);
          total_stats->stall_cycles_when_ready->addData(1);
          phase_stats->stall_cycles_when_ready->addData(1);
        }
      }
    }
  }

  if (!selected) {
    output_->verbose(CALL_INFO, 2, DEBUG_CLK, "no thread is ready\n");
  }
  return return_thread_id;
}

void DrvCore::executeReadyThread() {
  // select a ready thread to execute
  int thread_id = selectReadyThread();
  if (thread_id == NO_THREAD_READY) {
    addStallCycleStat(1);
    idle_cycles_++;
    return;
  }
  idle_cycles_ = 0;

  // execute the ready thread
  threads_[thread_id].execute(this);
  last_thread_ = thread_id;

  addBusyCycleStat(1);
}

void DrvCore::handleThreadStateAfterYield(DrvThread *thread) {
  std::shared_ptr<DrvAPI::DrvAPIThreadState> &state = thread->getAPIThread().getState();
  std::shared_ptr<DrvAPI::DrvAPIMem> mem_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIMem>(state);

  // handle memory requests
  if (mem_req) {
    // for now; do nothing
    memory_->sendRequest(this, thread, mem_req);
    return;
  }

  // handle set stage
  std::shared_ptr<DrvAPI::DrvAPISetStage> set_stage_req = std::dynamic_pointer_cast<DrvAPI::DrvAPISetStage>(state);
  if (set_stage_req) {
    output_->verbose(CALL_INFO, 1, DEBUG_CLK, "setting stage to %d\n", set_stage_req->getStage());
    stage_ = set_stage_req->getStage();
    set_stage_req->complete();
    return;
  }

  // handle increment phase
  std::shared_ptr<DrvAPI::DrvAPIIncrementPhase> increment_phase_req = std::dynamic_pointer_cast<DrvAPI::DrvAPIIncrementPhase>(state);
  if (increment_phase_req) {
    output_->verbose(CALL_INFO, 1, DEBUG_CLK, "phase increment\n");
    phase_ = (phase_ + 1) % phase_max_;

    // only one command processor outputs phase statistic
    if (pxn_ == 0 && pod_ == 0 && id_ == -1) {
      if (stage_ == DrvAPI::stage_t::STAGE_EXEC_COMP || stage_ == DrvAPI::stage_t::STAGE_EXEC_COMM) {
        if (phase_ == 0) {
          outputPhaseStatistics();
        }
      }
    }

    increment_phase_req->complete();
    return;
  }

  // handle nop
  std::shared_ptr<DrvAPI::DrvAPINop> nop_req = std::dynamic_pointer_cast<DrvAPI::DrvAPINop>(state);
  if (nop_req) {
    output_->verbose(CALL_INFO, 1, DEBUG_CLK, "thread %d nop for %d cycles\n", getThreadID(thread), nop_req->count());
    //TimeConverter *tc = getTimeConverter("1ns");
    loopback_->send(nop_req->count(), clocktc_, new DrvNopEvent(getThreadID(thread)));
    return;
  }

  // handle termination
  std::shared_ptr<DrvAPI::DrvAPITerminate> term_req = std::dynamic_pointer_cast<DrvAPI::DrvAPITerminate>(state);
  if (term_req) {
    output_->verbose(CALL_INFO, 1, DEBUG_CLK, "thread %d terminated\n", getThreadID(thread));
    done_--;
    return;
  }

  // fatal - unknown state
  output_->fatal(CALL_INFO, -1, "unknown thread state\n");
  return;
}

bool DrvCore::allDone() {
  return done_ == 0;
}

void DrvCore::updateTagCycles(int times) {
    // skip if these are null stats
    if (getStatisticLoadLevel() < TAG_EXECUTION_LOAD_LEVEL) {
        return;
    }

    for (auto &drv_thread : threads_) {
        if (stage_ == DrvAPI::stage_t::STAGE_EXEC_COMP || stage_ == DrvAPI::stage_t::STAGE_EXEC_COMM) {
          int tid = getThreadID(&drv_thread);
          auto &total_thread_stats = total_thread_stats_[tid];
          total_thread_stats.tag_cycles->addDataNTimes(times, drv_thread.getAPIThread().getTag());
        }
    }
}

bool DrvCore::clockTick(SST::Cycle_t cycle) {
  output_->verbose(CALL_INFO, 20, DEBUG_CLK, "tick!\n");
  // execute a ready thread
  executeReadyThread();
  if (allDone()) {
      primaryComponentOKToEndSim();
  }
  updateTagCycles(1);
  bool unregister = shouldUnregisterClock();
  core_on_ = (!unregister);
  if (unregister) {
    output_->verbose(CALL_INFO, 2, DEBUG_CLK, "unregistering clock\n");
    // save the time for statistics
    unregister_cycle_ = system_callbacks_->getCycleCount();
  }
  return unregister;
}

void DrvCore::handleLoopback(SST::Event *event) {
  DrvEvent *drv_event = dynamic_cast<DrvEvent*>(event);
  if (!drv_event) {
    output_->fatal(CALL_INFO, -1, "loopback event is not a thread\n");
  }
  // nop events
  DrvNopEvent *nop_event = dynamic_cast<DrvNopEvent*>(drv_event);
  if (nop_event) {
    output_->verbose(CALL_INFO, 2, DEBUG_LOOPBACK, "loopback event is a nop\n");
    DrvThread *thread = getThread(nop_event->tid);
    auto nop = std::dynamic_pointer_cast<DrvAPI::DrvAPINop>(thread->getAPIThread().getState());
    if (!nop) {
      output_->fatal(CALL_INFO, -1, "loopback event is not a nop\n");
    }
    nop->complete();
    assertCoreOn();
    delete event;
    return;
  }
}

/**
 * handle a memory request to control registers
 * the request is deleted by the caller
 */
void DrvCore::handleMMIOWriteRequest(Interfaces::StandardMem::Write* req) {
    output_->verbose(CALL_INFO, 0, DEBUG_MMIO,
                     "PXN %d: POD %d: Core %d: handling mmio write request\n"
                     ,pxn_,pod_,id_);
}
