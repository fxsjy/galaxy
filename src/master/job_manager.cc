// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "job_manager.h"

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/unordered_map.hpp>
#include <gflags/gflags.h>
#include "proto/agent.pb.h"
#include "proto/master.pb.h"
#include "proto/galaxy.pb.h"
#include "master_util.h"
#include "utils/resource_utils.h"
#include "timer.h"
#include <logging.h>

DECLARE_int32(master_agent_timeout);
DECLARE_int32(master_agent_rpc_timeout);
DECLARE_int32(master_query_period);
DECLARE_string(nexus_servers);
DECLARE_string(nexus_root_path);
DECLARE_string(labels_store_path);
DECLARE_string(jobs_store_path);
DECLARE_int32(master_pending_job_wait_timeout);

namespace baidu {
namespace galaxy {
JobManager::JobManager()
    : on_query_num_(0),
    pod_cv_(&mutex_){
    safe_mode_ = true;
    ScheduleNextQuery();
    state_to_stage_[kPodPending] = kStageRunning;
    state_to_stage_[kPodDeploying] = kStageRunning;
    state_to_stage_[kPodRunning] = kStageRunning;
    state_to_stage_[kPodTerminate] = kStageDeath;
    state_to_stage_[kPodError] = kStageDeath;
    BuildPodFsm();
    nexus_ = new ::galaxy::ins::sdk::InsSDK(FLAGS_nexus_servers);
}

JobManager::~JobManager() {
    delete nexus_;
}

Status JobManager::LabelAgents(const LabelCell& label_cell) {
    AgentSet new_label_set;
    for (int i = 0; i < label_cell.agents_endpoint_size(); ++i) {
        new_label_set.insert(label_cell.agents_endpoint(i)); 
    }
    // TODO add nexus lock
    bool save_ok = SaveLabelToNexus(label_cell);
    if (!save_ok) {
        return kLabelAgentFail;
    }
    MutexLock lock(&mutex_); 
    AgentSet& old_label_set = labels_[label_cell.label()];
    AgentSet to_remove;
    AgentSet to_add;
    MasterUtil::SetDiff(old_label_set, 
                        new_label_set, &to_remove, &to_add);
    LOG(INFO, "label %s old label %u, new label %u, to_remove %u, to_add %u",
            label_cell.label().c_str(), 
            old_label_set.size(), 
            new_label_set.size(), 
            to_remove.size(), 
            to_add.size()); 
    // TODO may use internal AgentInfo with set 
    for (AgentSet::iterator it = to_remove.begin(); 
                                it != to_remove.end(); ++it) {
        boost::unordered_map<AgentAddr, LabelSet>::iterator 
                lab_it = agent_labels_.find(*it);
        if (lab_it == agent_labels_.end()) {
            continue; 
        }
        LOG(DEBUG, "%s remove label %s", (*it).c_str(), label_cell.label().c_str());
        lab_it->second.erase(label_cell.label());
        agent_labels_[*it].erase(label_cell.label()); 
        // update Version
        std::map<AgentAddr, AgentInfo*>::iterator agent_it 
                                            = agents_.find(*it); 
        if (agent_it != agents_.end()) {
            AgentInfo* agent_ptr = agent_it->second;
            agent_ptr->set_version(agent_ptr->version() + 1);
            MasterUtil::ResetLabels(agent_ptr, agent_labels_[*it]);
        }
    }
    for (AgentSet::iterator it = to_add.begin();
                                it != to_add.end(); ++it) {
        agent_labels_[*it].insert(label_cell.label()); 
        // update Version
        LOG(DEBUG, "%s add label %s", (*it).c_str(), label_cell.label().c_str());
        std::map<AgentAddr, AgentInfo*>::iterator agent_it 
                                            = agents_.find(*it);
        if (agent_it != agents_.end()) {
            AgentInfo* agent_ptr = agent_it->second;
            agent_ptr->set_version(agent_ptr->version() + 1);
            MasterUtil::ResetLabels(agent_ptr, agent_labels_[*it]);
        }
    }

    labels_[label_cell.label()] = new_label_set;
    return kOk;
}

Status JobManager::Add(const JobId& job_id, const JobDescriptor& job_desc) {
    Job* job = new Job();
    job->state_ = kJobNormal;
    job->desc_.CopyFrom(job_desc);
    job->id_ = job_id;
    // add default version
    if (job->desc_.pod().has_version()
       || job->desc_.pod().version().empty()) {
        job->desc_.mutable_pod()->set_version("1.0.0");
    }
    job->pod_desc_[job->desc_.pod().version()] = job->desc_.pod();
    job->latest_version = job->desc_.pod().version();
    // disable update default
    job->update_state_ = kUpdateSuspend;
    // TODO add nexus lock
    bool save_ok = SaveToNexus(job);
    if (!save_ok) {
        return kJobSubmitFail;
    }
    MutexLock lock(&mutex_); 
    jobs_[job_id] = job;
    FillPodsToJob(job);
    pod_cv_.Signal();
    LOG(INFO, "job %s submitted by user: %s, with deploy_step %d, replica %d , pod version %s",
      job_id.c_str(), 
      job_desc.user().c_str(),
      job_desc.deploy_step(),
      job_desc.replica(),
      job->latest_version.c_str());
    return kOk;
}

Status JobManager::Update(const JobId& job_id, const JobDescriptor& job_desc) {
    Job job;
    bool replica_changed = false;
    bool pod_desc_changed = false;
    {
        MutexLock lock(&mutex_);
        std::map<JobId, Job*>::iterator it;
        it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            LOG(WARNING, "update job failed, job not found: %s", job_id.c_str());
            return kJobNotFound;
        }
        job = *(it->second);
        bool ok = HandleUpdateJob(job_desc, &job, &replica_changed, &pod_desc_changed);
        if (!ok) {
            LOG(WARNING, "update job %s failed for handle update job error", job_id.c_str());
            return kJobUpdateFail;
        }
    }
    // TODO add nexus lock
    bool save_ok = SaveToNexus(&job);
    if (!save_ok) {
        return kJobUpdateFail;
    }
    MutexLock lock(&mutex_);
    std::map<JobId, Job*>::iterator it;
    it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        LOG(WARNING, "update job failed, job not found: %s", job_id.c_str());
        return kJobNotFound;
    }
    int32_t old_replica = it->second->desc_.replica();
    bool ok = HandleUpdateJob(job_desc, it->second, 
                      &replica_changed, &pod_desc_changed);
    if (!ok) {
        LOG(WARNING, "update job %s failed for handle update job error", job_id.c_str());
        return kJobUpdateFail;
    }
    if (old_replica < job_desc.replica()) {
        FillPodsToJob(it->second);
    } else if (old_replica > job_desc.replica()) {
        scale_down_jobs_.insert(it->second->id_);
    }
    if (pod_desc_changed) {
        need_update_jobs_.insert(job_id);
    }

    if (pod_desc_changed || replica_changed) {
        pod_cv_.Signal();
    }
    LOG(INFO, "job desc updated succes: %s", job_desc.name().c_str());
    return kOk;
}

bool JobManager::HandleUpdateJob(const JobDescriptor& desc, Job* job,
                                 bool* replica_change,
                                 bool* pod_desc_change) {
    mutex_.AssertHeld();
    if (replica_change == NULL || pod_desc_change == NULL) {
        return false;
    }
    *pod_desc_change = false;
    *replica_change = false;
    if (desc.pod().version() != job->latest_version) {
        job->latest_version = desc.pod().version();
        job->desc_.mutable_pod()->CopyFrom(desc.pod());
        job->pod_desc_[desc.pod().version()] = desc.pod();
        *pod_desc_change = true;
    }
    
    if (desc.replica() != job->desc_.replica()) {
        job->desc_.set_replica(desc.replica());
        *replica_change = true;
    }
    return true;
}

Status JobManager::Terminte(const JobId& jobid) {
    Job job;
    {
        MutexLock lock(&mutex_);
        std::map<JobId, Job*>::iterator it = jobs_.find(jobid);
        if (it == jobs_.end()) {
            return kJobNotFound;
        }
        job = *(it->second);
        job.state_ = kJobTerminated;
        job.desc_.set_replica(0);
    }
    // TODO add nexus lock
    bool save_ok = SaveToNexus(&job);
    if (!save_ok) {
        return kJobTerminateFail;
    }
    MutexLock lock(&mutex_);
    std::map<JobId, Job*>::iterator it = jobs_.find(jobid);
    if (it == jobs_.end()) {
        return kJobNotFound;
    }
    it->second->desc_.set_replica(0);
    if (it->second->pods_.size() > 0) { 
        scale_down_jobs_.insert(it->second->id_);
    }
    it->second->state_ = kJobTerminated;
    pod_cv_.Signal();
    return kOk;
}

void JobManager::FillPodsToJob(Job* job) {
    mutex_.AssertHeld();
    bool need_scale_up = false;
    job->pod_desc_[job->desc_.pod().version()] = job->desc_.pod();
    for(int i = job->pods_.size(); i < job->desc_.replica(); i++) {
        PodId pod_id = MasterUtil::UUID();
        PodStatus* pod_status = new PodStatus();
        pod_status->set_podid(pod_id);
        pod_status->set_jobid(job->id_);
        pod_status->set_state(kPodPending);
        pod_status->set_stage(kStagePending);
        pod_status->set_version(job->latest_version);
        job->pods_[pod_id] = pod_status;
        need_scale_up = true;
    }
    if (need_scale_up) {
        LOG(INFO, "move job %s to scale up queue", job->id_.c_str());
        scale_up_jobs_.insert(job->id_);
    }
}

void JobManager::FillAllJobs() {
    mutex_.AssertHeld();
    std::map<JobId, Job*>::iterator it;
    for (it = jobs_.begin(); it != jobs_.end(); ++it) {
        JobId job_id = it->first;
        Job* job = it->second;
        if (it->second->state_ == kJobTerminated) {
            scale_down_jobs_.insert(job_id);
            continue;
        }
        FillPodsToJob(job);
    }
}

void JobManager::ReloadJobInfo(const JobInfo& job_info) {
    Job* job = new Job();
    job->state_ = job_info.state();
    job->desc_.CopyFrom(job_info.desc());
    std::string job_id = job_info.jobid();
    job->id_ = job_id;
    job->update_state_ = job_info.update_state();
    job->latest_version = job_info.latest_version();
    for (int32_t i = 0; i < job_info.pod_descs_size(); i++) {
        const PodDescriptor& desc = job_info.pod_descs(i);
        job->pod_desc_[desc.version()] = desc;
    }
    MutexLock lock(&mutex_);
    jobs_[job_id] = job;
}


void JobManager::KillPodCallback(const std::string& podid, const std::string& jobid,
                                 const std::string& endpoint,
                                 const KillPodRequest* request, KillPodResponse* response, 
                                 bool failed, int /*error*/) {
    boost::scoped_ptr<const KillPodRequest> request_ptr(request);
    boost::scoped_ptr<KillPodResponse> response_ptr(response);
    Status status = response->status();
    if (failed || status != kOk) {
        LOG(INFO, "Kill pod [%s %s] on [%s] fail: %d", jobid.c_str(),
            podid.c_str(), endpoint.c_str(), status);
        return;
    }
    LOG(INFO, "send kill cmd for pod [%s %s] on [%s] success", jobid.c_str(),
        podid.c_str(), endpoint.c_str());
}

void JobManager::GetPendingPods(JobInfoList* pending_pods,
                                int32_t max_scale_up_size,
                                JobInfoList* scale_down_pods,
                                int32_t max_scale_down_size,
                                JobInfoList* need_update_jobs,
                                int32_t max_need_update_job_size,
                                ::google::protobuf::Closure* done) {
    MutexLock lock(&mutex_);
    if (safe_mode_) {
        done->Run();
        return;
    }
    if (scale_down_jobs_.size() <= 0 
        && scale_up_jobs_.size() <= 0
        && need_update_jobs_.size() <= 0) {
        pod_cv_.TimeWait(FLAGS_master_pending_job_wait_timeout);
    }
    ProcessScaleDown(scale_down_pods, max_scale_down_size);
    ProcessScaleUp(pending_pods, max_scale_up_size);
    ProcessUpdateJob(need_update_jobs, max_need_update_job_size);
    done->Run();
}

void JobManager::ProcessUpdateJob(JobInfoList* need_update_jobs,
                                  int32_t max_need_update_job_size) {
    mutex_.AssertHeld();
    std::set<JobId> should_rm_from_update;
    std::set<JobId>::iterator jobid_it = need_update_jobs_.begin();
    int32_t job_count = 0;
    for (; jobid_it != need_update_jobs_.end(); ++jobid_it) {
        if (job_count >= max_need_update_job_size) {
            return;
        }
        std::map<JobId, Job*>::iterator job_it = jobs_.find(*jobid_it);
        if (job_it == jobs_.end()) {
            should_rm_from_update.insert(*jobid_it);
            continue;
        }
        Job* job = job_it->second;
        // check if job still needs update
        std::map<PodId, PodStatus*>::iterator pod_it = job->pods_.begin();
        bool need_update = false;
        for (; pod_it != job->pods_.end(); ++pod_it) {
            if (pod_it->second->version() != job->latest_version) {
                need_update = true;
                break;
            }
        }
        if (need_update) {
            job_count++;
            LOG(INFO, "add job %s to update queue", job->id_.c_str());
            pod_it = job->pods_.begin();
            JobInfo* job_info = need_update_jobs->Add();
            for (; pod_it != job->pods_.end(); ++pod_it) {
                PodStatus* pod = job_info->add_pods();
                pod->CopyFrom(*(pod_it->second));
            }
            job_info->set_jobid(job->id_);
            job_info->set_latest_version(job->latest_version);
            std::map<Version, PodDescriptor>::iterator v_it = job->pod_desc_.begin();
            for (; v_it != job->pod_desc_.end(); ++v_it) {
                PodDescriptor* pod_desc = job_info->add_pod_descs();
                pod_desc->CopyFrom(v_it->second);
            }
            job_info->set_update_state(kUpdateNormal);
            job->update_state_ = kUpdateNormal;
        }else {
            job->update_state_ = kUpdateSuspend;
            should_rm_from_update.insert(*jobid_it);
            LOG(INFO, "job %s leave update state", job->id_.c_str());
        }
    }
    jobid_it = should_rm_from_update.begin();
    for (; jobid_it != should_rm_from_update.end(); ++jobid_it) {
        need_update_jobs_.erase(*jobid_it);
    }
}

void JobManager::ProcessScaleDown(JobInfoList* scale_down_pods,
                                  int32_t max_scale_down_size) {
    mutex_.AssertHeld();
    std::set<JobId> should_rm_from_scale_down;
    std::set<JobId> should_been_cleaned;
    std::set<JobId>::iterator jobid_it = scale_down_jobs_.begin();
    int32_t job_count = 0;
    for (;jobid_it != scale_down_jobs_.end() && job_count < max_scale_down_size; 
      ++jobid_it) {
        std::map<JobId, Job*>::iterator job_it = jobs_.find(*jobid_it);
        if (job_it == jobs_.end()) {
            continue;
        }
        Job* job = job_it->second;
        size_t replica = job->desc_.replica();
        if (replica >= job->pods_.size()) {
            if (job->pods_.size() == 0 
                && job->state_ == kJobTerminated) {
                should_been_cleaned.insert(*jobid_it);
            }else {
                should_rm_from_scale_down.insert(*jobid_it);
            }
            continue;
        } 
        int32_t scale_down_count = job->pods_.size() - job->desc_.replica();
        std::set<JobId>::iterator su_jobid_it  = scale_up_jobs_.find(*jobid_it);
        // scale down pods with pending stage 
        std::vector<PodStatus*> pods_will_been_removed;
        if (su_jobid_it != scale_up_jobs_.end()) {
            std::map<PodId, PodStatus*>::iterator pod_it = job->pods_.begin();
            for (; pod_it != job->pods_.end() && scale_down_count > 0;
              ++pod_it) {
                if (pod_it->second->stage() != kStagePending) {
                    continue;
                }
                --scale_down_count;
                pods_will_been_removed.push_back(pod_it->second);
            }
        } 
        for (size_t i = 0; i < pods_will_been_removed.size(); ++i) {
            ChangeStage(kStageRemoved, pods_will_been_removed[i], job);
        }
        LOG(INFO, "process scale down job %s, replica %d, pods size %u , remove pending pods count %u, scheduler scale down count %d",
            job->id_.c_str(),
            job->desc_.replica(),
            job->pods_.size(),
            pods_will_been_removed.size(),
            scale_down_count);
        // scale down pods with running stage or death stage
        if (scale_down_count > 0) {
            job_count++;
            JobInfo* job_info = scale_down_pods->Add();
            job_info->mutable_desc()->CopyFrom(job_it->second->desc_);
            job_info->set_jobid(job_it->second->id_);
            std::map<PodId, PodStatus*>::const_iterator jt;
            for (jt = job->pods_.begin();
                 jt != job->pods_.end(); ++jt) {
                PodStatus* pod_status = jt->second;
                PodStatus* new_pod_status = job_info->add_pods();
                new_pod_status->CopyFrom(*pod_status);
            }
        }
    }
    for (std::set<JobId>::iterator it = should_rm_from_scale_down.begin();
         it != should_rm_from_scale_down.end(); ++it) {
        scale_down_jobs_.erase(*it);
    }
    for (std::set<JobId>::iterator it = should_been_cleaned.begin();
         it != should_been_cleaned.end(); ++it) {
        scale_down_jobs_.erase(*it);
        CleanJob(*it);
    }
}

void JobManager::CleanJob(const JobId& jobid) {
    mutex_.AssertHeld();
    LOG(INFO, "clean job %s from master", jobid.c_str());
    std::map<JobId, Job*>::iterator it = jobs_.find(jobid);
    if (it != jobs_.end()) {
        Job* job = it->second;
        jobs_.erase(jobid);
        thread_pool_.AddTask(boost::bind(&JobManager::DeleteFromNexus, this, jobid));
        delete job;
    }
}

void JobManager::ProcessScaleUp(JobInfoList* scale_up_pods,
                                int32_t max_scale_up_size) {
    mutex_.AssertHeld();
    int job_count = 0;
    std::set<JobId>::iterator jobid_it = scale_up_jobs_.begin();
    std::set<JobId> would_been_removed;
    for (;jobid_it != scale_up_jobs_.end() && job_count < max_scale_up_size;
         ++jobid_it) {
        std::map<JobId, Job*>::iterator job_it = jobs_.find(*jobid_it);
        if (job_it == jobs_.end()) {
            would_been_removed.insert(*jobid_it);
            continue;
        }
        Job* job = job_it->second;
        std::map<PodId, PodStatus*>::iterator pod_it = job->pods_.begin();
        // check if job need scale up
        bool need_scale_up = false;
        for (; pod_it != job->pods_.end(); ++pod_it) {
            if (pod_it->second->stage() == kStagePending) {
                need_scale_up = true;
                break;
            }
        }
        if (!need_scale_up) {
            would_been_removed.insert(*jobid_it);
            continue;
        }
        JobInfo* job_info = scale_up_pods->Add();
        job_info->set_jobid(job->id_);
        job_info->mutable_desc()->CopyFrom(job->desc_);
        // copy all pod
        for (pod_it= job->pods_.begin(); pod_it != job->pods_.end(); ++pod_it) {
            PodStatus* pod_status = job_info->add_pods();
            pod_status->CopyFrom(*(pod_it->second));
        }
        job_count++;
    }
    jobid_it = would_been_removed.begin();
    for (; jobid_it != would_been_removed.end(); ++jobid_it) {
        scale_up_jobs_.erase(*jobid_it);
    }
}

Status JobManager::Propose(const ScheduleInfo& sche_info) {
    MutexLock lock(&mutex_);
    const std::string& jobid = sche_info.jobid();
    const std::string& podid = sche_info.podid();
    const std::string& endpoint = sche_info.endpoint();
    std::map<JobId, Job*>::iterator job_it = jobs_.find(jobid);
    if (job_it == jobs_.end()) { 
        LOG(WARNING, "propose fail, no such job: %s", jobid.c_str());
        return kJobNotFound;
    }
    Job* job = job_it->second;
    std::map<PodId, PodStatus*>::iterator pod_it = job->pods_.find(sche_info.podid());
    if (pod_it == job->pods_.end()) {
        LOG(WARNING, "propose fails, no such pod %s in job %s",
          podid.c_str(), jobid.c_str());
        return kPodNotFound;
    }
    PodStatus* pod = pod_it->second;
    if (sche_info.action() == kTerminate 
        && job->state_ == kJobTerminated) {
        ChangeStage(kStageRemoved, pod, job);
        return kOk;
    } else if (sche_info.action() == kLaunch
               && job->state_ != kJobTerminated) {
        if (pod->stage() != kStagePending) {
            LOG(WARNING, "propose fails for pod been in invalide stage %s, require stage kStgePending", PodStage_Name(pod->stage()).c_str());
            return kInputError; 
        }
        std::map<AgentAddr, AgentInfo*>::iterator at = agents_.find(endpoint);
        if (at == agents_.end()) {
            LOG(WARNING, "propose fail, no such agent: %s", endpoint.c_str());
            return kAgentNotFound;
        }
        AgentInfo* agent = at->second;
        if (agent->state() == kDead) {
            LOG(WARNING, "propose fails, agent %s been kDead state", endpoint.c_str());
            return kInputError;

        }
        Status feasible_status = AcquireResource(*pod, agent);
        if (feasible_status != kOk) {
            LOG(INFO, "propose fail, no resource, error code:[%d]", feasible_status);
            return feasible_status;
        }
        agent->set_version(agent->version() + 1);
        pod->set_endpoint(sche_info.endpoint());
        ChangeStage(kStageRunning, pod, job_it->second);
        LOG(INFO, "propose success, %s will be run on %s",
        podid.c_str(), endpoint.c_str());
        return kOk;
    } else if(sche_info.action() == kTerminate
              && job->state_ != kJobTerminated 
              && job->update_state_ == kUpdateNormal) {
        LOG(INFO, "update pod %s of job %s", 
                  pod->podid().c_str(),
                  job->id_.c_str());
        ChangeStage(kStageDeath, pod, job);
        return kOk;
    }
    return kInputError;
}

Status JobManager::AcquireResource(const PodStatus& pod, AgentInfo* agent) {
    mutex_.AssertHeld();
    Resource pod_requirement;
    GetPodRequirement(pod, &pod_requirement);
    // calc agent unassigned resource 
    Resource unassigned;
    unassigned.CopyFrom(agent->total());
    bool ret = ResourceUtils::Alloc(agent->assigned(), unassigned);
    if (!ret) {
        return kQuota;
    }
    if (!MasterUtil::FitResource(pod_requirement, unassigned)) {
        return kQuota;
    }
    return kOk;
}


void JobManager::ReclaimResource(const PodStatus& pod, AgentInfo* agent) {
    mutex_.AssertHeld();
    Resource pod_requirement;
    GetPodRequirement(pod, &pod_requirement);
    MasterUtil::SubstractResource(pod_requirement, agent->mutable_assigned());
}

void JobManager::GetPodRequirement(const PodStatus& pod, Resource* requirement) {
    Job* job = jobs_[pod.jobid()];
    const PodDescriptor& pod_desc = job->desc_.pod();
    requirement->CopyFrom(pod_desc.requirement());
}

void JobManager::CalculatePodRequirement(const PodDescriptor& pod_desc,
                                         Resource* pod_requirement) {
    for (int32_t i = 0; i < pod_desc.tasks_size(); i++) {
        const TaskDescriptor& task_desc = pod_desc.tasks(i);
        const Resource& task_requirement = task_desc.requirement();
        pod_requirement->set_millicores(pod_requirement->millicores() + task_requirement.millicores());
        pod_requirement->set_memory(pod_requirement->memory() + task_requirement.memory());
        for (int32_t j = 0; j < task_requirement.ports_size(); j++) {
            pod_requirement->add_ports(task_requirement.ports(j));
        }
    }
}

void JobManager::KeepAlive(const std::string& agent_addr) {
    if (agent_addr == "") {
        LOG(WARNING, "ignore heartbeat with empty endpoint");
        return;
    }
    {
        MutexLock lock(&mutex_);
        if (agents_.find(agent_addr) == agents_.end()) {
            LOG(INFO, "new agent added: %s", agent_addr.c_str());
            AgentInfo* agent = new AgentInfo();
            agent->set_version(0);
            agents_[agent_addr] = agent;
            MasterUtil::ResetLabels(agent, agent_labels_[agent_addr]);
        }
        AgentInfo* agent = agents_[agent_addr];
        agent->set_state(kAlive);
        agent->set_endpoint(agent_addr);
    }

    MutexLock lock(&mutex_timer_);
    LOG(DEBUG, "receive heartbeat from %s", agent_addr.c_str());
    int64_t timer_id;
    if (agent_timer_.find(agent_addr) != agent_timer_.end()) {
        timer_id = agent_timer_[agent_addr];
        bool cancel_ok = death_checker_.CancelTask(timer_id);
        if (!cancel_ok) {
            LOG(WARNING, "agent is offline, agent: %s", agent_addr.c_str());
            
        }
    }
    timer_id = death_checker_.DelayTask(FLAGS_master_agent_timeout,
                                        boost::bind(&JobManager::HandleAgentOffline,
                                                    this,
                                                    agent_addr));
    agent_timer_[agent_addr] = timer_id;
}

void JobManager::HandleAgentOffline(const std::string agent_addr) {
    LOG(WARNING, "agent is offline: %s", agent_addr.c_str());
    MutexLock lock(&mutex_);
    std::map<AgentAddr, AgentInfo*>::iterator a_it = agents_.find(agent_addr);
    if (a_it == agents_.end()) {
        LOG(INFO, "no such agent %s", agent_addr.c_str());
        return;
    }
    a_it->second->set_state(kDead);
    PodMap& agent_pods = pods_on_agent_[agent_addr];
    PodMap::iterator it = agent_pods.begin();
    for (; it != agent_pods.end(); ++it) {
        std::map<PodId, PodStatus*>::iterator jt = it->second.begin();
        for (; jt != it->second.end(); ++jt) {
            PodStatus* pod = jt->second; 
            std::map<JobId, Job*>::iterator job_it = jobs_.find(pod->jobid());
            if (job_it ==  jobs_.end()) {
                continue;
            }
            pod->set_stage(kStageDeath);
            ChangeStage(kStagePending, pod, job_it->second);
        }
    }
    LOG(INFO, "agent is dead: %s", agent_addr.c_str());
}

void JobManager::RunPod(const PodDescriptor& desc, PodStatus* pod) {
    mutex_.AssertHeld();
    RunPodRequest* request = new RunPodRequest;
    RunPodResponse* response = new RunPodResponse;
    request->set_podid(pod->podid());
    request->mutable_pod()->CopyFrom(desc);
    request->set_jobid(pod->jobid());
    Agent_Stub* stub;
    const AgentAddr& endpoint = pod->endpoint();
    rpc_client_.GetStub(endpoint, &stub);
    boost::function<void (const RunPodRequest*, RunPodResponse*, bool, int)> run_pod_callback;
    run_pod_callback = boost::bind(&JobManager::RunPodCallback, this, pod, endpoint,
                                   _1, _2, _3, _4);
    rpc_client_.AsyncRequest(stub, &Agent_Stub::RunPod, request, response,
                             run_pod_callback, FLAGS_master_agent_rpc_timeout, 0);
    delete stub;
}


void JobManager::RunPodCallback(PodStatus* pod, AgentAddr endpoint,
                                const RunPodRequest* request,
                                RunPodResponse* response,
                                bool failed, int /*error*/) {
    boost::scoped_ptr<const RunPodRequest> request_ptr(request);
    boost::scoped_ptr<RunPodResponse> response_ptr(response);
    MutexLock lock(&mutex_);
    const std::string& jobid = pod->jobid();
    const std::string& podid = pod->podid();
    Status status = response->status();
    std::map<JobId, Job*>::iterator job_it = jobs_.find(jobid);
    if (job_it == jobs_.end()) {
        LOG(WARNING, "job %s does not exist in master", jobid.c_str());
        return;
    }
    if (failed || status != kOk) {
        LOG(WARNING, "run pod [%s %s] on [%s] fail: %d", jobid.c_str(),
            podid.c_str(), endpoint.c_str(), status);
        pod->set_stage(kStageDeath);
        ChangeStage(kStagePending, pod, job_it->second);
    } else {
        std::map<PodId, PodStatus*>::iterator pod_it = job_it->second->pods_.find(podid);
        if (pod_it == job_it->second->pods_.end()) {
            LOG(WARNING, "pod %s does not exist in master", podid.c_str());
            return;
        }
        PodStatus* pod_status = pod_it->second;
        agent_sequence_ids_[pod->endpoint()]++;
        pods_on_agent_[pod->endpoint()][jobid].insert(std::make_pair(pod_status->podid(),pod_status));
        LOG(INFO, "run pod [%s %s] on [%s] success", jobid.c_str(),
          podid.c_str(), 
          endpoint.c_str());
    }
}


void JobManager::ScheduleNextQuery() {
    thread_pool_.DelayTask(FLAGS_master_query_period, boost::bind(&JobManager::Query, this));
}

void JobManager::Query() {
    MutexLock lock(&mutex_);
    assert(on_query_num_ == 0);
    std::map<AgentAddr, AgentInfo*>::iterator it;
    for (it = agents_.begin(); it != agents_.end(); ++it) {
        AgentInfo* agent = it->second;
        if (agent->state() == kDead) {
            continue;
        }
        QueryAgent(agent);
    }
    LOG(INFO, "query %lld agents", on_query_num_);
    if (on_query_num_ == 0) {
        ScheduleNextQuery();
    }
}

void JobManager::QueryAgent(AgentInfo* agent) {
    mutex_.AssertHeld();
    const AgentAddr& endpoint = agent->endpoint();
    int64_t seq_id = agent_sequence_ids_[endpoint];
    if (agent->state() != kAlive) {
        LOG(DEBUG, "ignore dead agent [%s]", endpoint.c_str());
        return;
    }
    QueryRequest* request = new QueryRequest;
    QueryResponse* response = new QueryResponse;

    Agent_Stub* stub;
    rpc_client_.GetStub(endpoint, &stub);
    boost::function<void (const QueryRequest*, QueryResponse*, bool, int)> query_callback;
    query_callback = boost::bind(&JobManager::QueryAgentCallback, this,
                                 endpoint, _1, _2, _3, _4, seq_id);
    rpc_client_.AsyncRequest(stub, &Agent_Stub::Query, request, response,
                             query_callback, FLAGS_master_agent_rpc_timeout, 0);
    delete stub;
    on_query_num_++;
}

void JobManager::QueryAgentCallback(AgentAddr endpoint, const QueryRequest* request,
                                    QueryResponse* response, bool failed, int /*error*/,
                                    int64_t seq_id_at_query) {
    boost::scoped_ptr<const QueryRequest> request_ptr(request);
    boost::scoped_ptr<QueryResponse> response_ptr(response);
    MutexLock lock(&mutex_);
    bool first_query_on_agent = false;
    if (queried_agents_.find(endpoint) == queried_agents_.end()) {
        first_query_on_agent = true;
        LOG(INFO, "first query callback for agent: %s", endpoint.c_str());
        queried_agents_.insert(endpoint);
    }
    
    if (--on_query_num_ == 0) {
        ScheduleNextQuery();
    }
    std::map<AgentAddr, AgentInfo*>::iterator it = agents_.find(endpoint);
    if (it == agents_.end()) {
        LOG(FATAL, "query agent [%s] not exist", endpoint.c_str());
        return;
    }
    int64_t current_seq_id = agent_sequence_ids_[endpoint];
    if (seq_id_at_query < current_seq_id) {
        LOG(FATAL, "outdated query callback, just ignore it. %ld < %ld, %s", 
            seq_id_at_query, current_seq_id, endpoint.c_str());
        return;
    }
    Status status = response->status();
    if (failed || status != kOk) {
        LOG(INFO, "query agent [%s] fail: %d", endpoint.c_str(), status);
        return;
    }
    const AgentInfo& report_agent_info = response->agent();
    LOG(INFO, "agent %s stat: mem total %ld, cpu total %d, mem assigned %ld, cpu assigend %d, mem used %ld , cpu used %d, pod size %d ",
        report_agent_info.endpoint().c_str(),
        report_agent_info.total().memory(), report_agent_info.total().millicores(),
        report_agent_info.assigned().memory(), report_agent_info.assigned().millicores(),
        report_agent_info.used().memory(), report_agent_info.used().millicores(),
        report_agent_info.pods_size());
    AgentInfo* agent = it->second;
    UpdateAgentVersion(agent, report_agent_info);
    // copy only needed 
    agent->mutable_total()->CopyFrom(report_agent_info.total()); 
    agent->mutable_assigned()->CopyFrom(report_agent_info.assigned());
    agent->mutable_used()->CopyFrom(report_agent_info.used());
    agent->mutable_pods()->CopyFrom(report_agent_info.pods());
    PodMap pods_not_on_agent = pods_on_agent_[endpoint]; // this is a copy
    for (int32_t i = 0; i < report_agent_info.pods_size(); i++) {
        const PodStatus& report_pod_info = report_agent_info.pods(i);
        const JobId& jobid = report_pod_info.jobid();
        const PodId& podid = report_pod_info.podid(); 
        LOG(INFO, "the pod %s of job %s on agent %s state %s",
                  podid.c_str(), 
                  jobid.c_str(), 
                  report_agent_info.endpoint().c_str(),
                  PodState_Name(report_pod_info.state()).c_str()); 
        // validate job 
        std::map<JobId, Job*>::iterator job_it = jobs_.find(jobid);
        if (job_it == jobs_.end()) {
            LOG(WARNING, "the job %s from agent %s does not exist in master",
               jobid.c_str(), 
               report_agent_info.endpoint().c_str());
            PodStatus fake_pod;
            fake_pod.CopyFrom(report_pod_info);
            fake_pod.set_endpoint(report_agent_info.endpoint());
            fake_pod.set_stage(state_to_stage_[fake_pod.state()]);
            ChangeStage(kStageRemoved, &fake_pod, NULL);
            continue;
        }
        // for recovering
        if (first_query_on_agent 
            && jobs_[jobid]->pods_.find(podid) == jobs_[jobid]->pods_.end()) {
            PodStatus* pod = new PodStatus();
            pod->CopyFrom(report_pod_info);
            pod->set_stage(state_to_stage_[pod->state()]);
            pod->set_endpoint(report_agent_info.endpoint());
            jobs_[jobid]->pods_[podid] = pod;
            pods_not_on_agent[jobid][podid] = pod;
            pods_on_agent_[endpoint][jobid][podid] = pod;
        }
        // validate pod
        Job* job = job_it->second;
        std::map<PodId, PodStatus*>::iterator p_it = job->pods_.find(podid);
        if (p_it == job->pods_.end()) {
            // TODO should kill this pod
            LOG(WARNING, "the pod %s from agent %s does not exist in master",
               podid.c_str(), report_agent_info.endpoint().c_str());
            PodStatus fake_pod;
            fake_pod.CopyFrom(report_pod_info);
            fake_pod.set_endpoint(report_agent_info.endpoint());
            fake_pod.set_stage(state_to_stage_[fake_pod.state()]);
            ChangeStage(kStageRemoved, &fake_pod, NULL);
            continue;
        } 
        // update pod in master
        PodStatus* pod = jobs_[jobid]->pods_[podid];
        pod->mutable_status()->CopyFrom(report_pod_info.status());
        pod->mutable_resource_used()->CopyFrom(report_pod_info.resource_used());
        pod->set_state(report_pod_info.state());
        pod->set_endpoint(report_agent_info.endpoint());
        pods_not_on_agent[jobid].erase(podid);
        if (pods_not_on_agent[jobid].size() == 0) {
            pods_not_on_agent.erase(jobid);
        }
        ChangeStage(state_to_stage_[pod->state()], pod, job);
    }

    PodMap::iterator j_it = pods_not_on_agent.begin();
    for (; j_it != pods_not_on_agent.end(); ++j_it) {
        std::map<JobId, Job*>::iterator job_it = jobs_.find(j_it->first);
        if (job_it == jobs_.end()) {
            continue;
        }
        std::map<PodId, PodStatus*>::iterator p_it = j_it->second.begin();
        for (; p_it != j_it->second.end(); ++p_it) {
            LOG(INFO, "pod %s in master but not in agent, stage in master is %s ", 
              p_it->second->podid().c_str(),
              PodStage_Name(p_it->second->stage()).c_str());
            ChangeStage(kStagePending, p_it->second, job_it->second);
        }
    }

    if (queried_agents_.size() == agents_.size() && safe_mode_) {
        FillAllJobs();
        safe_mode_ = false;
        LOG(INFO, "master leave safe mode");
    }
}

void JobManager::UpdateAgentVersion(AgentInfo* old_agent_info,
                                    const AgentInfo& new_agent_info) {
  
    int old_version = old_agent_info->version();
    // check assigned
    do {
        int32_t check_assigned = ResourceUtils::Compare(
                        old_agent_info->assigned(),
                        new_agent_info.assigned());
        if (check_assigned != 0) {
            old_agent_info->set_version(old_agent_info->version() + 1);
            break;
        }

        // check used
        int32_t check_used = ResourceUtils::Compare(
                        old_agent_info->used(),
                        new_agent_info.used());
        if (check_used != 0) {
            old_agent_info->set_version(old_agent_info->version() + 1);
            break;
        }

        // check total resource 
        int32_t check_total = ResourceUtils::Compare(
                        old_agent_info->total(), 
                        new_agent_info.total());
        if (check_total != 0) {
            old_agent_info->set_version(old_agent_info->version() + 1);
            break;
        } 
    } while(0);

    LOG(INFO, "agent %s change version from %d to %d", 
              old_agent_info->endpoint().c_str(), 
              old_version,
              old_agent_info->version());
    return; 
}

void JobManager::GetAgentsInfo(AgentInfoList* agents_info) {
    MutexLock lock(&mutex_);
    if (safe_mode_) {
        return;
    }

    std::map<AgentAddr, AgentInfo*>::iterator it;
    for (it = agents_.begin(); it != agents_.end(); ++it) {
        AgentInfo* agent = it->second;
        agents_info->Add()->CopyFrom(*agent);
    }
}

void JobManager::GetAliveAgentsInfo(AgentInfoList* agents_info) {
    MutexLock lock(&mutex_);
    std::map<AgentAddr, AgentInfo*>::iterator it;
    for (it = agents_.begin(); it != agents_.end(); ++it) {
        AgentInfo* agent = it->second;
        if (agent->state() != kAlive) {
            continue;
        }
        agents_info->Add()->CopyFrom(*agent);
    }
}

void JobManager::GetAliveAgentsByDiff(const DiffVersionList& versions,
                                      AgentInfoList* agents_info,
                                      StringList* deleted_agents,
                                      ::google::protobuf::Closure* done) {
    MutexLock lock(&mutex_);
    LOG(INFO, "get alive agents by diff , diff count %u", versions.size());
    // ms
    long now_time = ::baidu::common::timer::get_micros() / 1000;
    boost::unordered_map<AgentAddr, int32_t> agents_ver_map;
    for (int i = 0; i < versions.size(); i++) {
        agents_ver_map.insert(std::make_pair(versions.Get(i).endpoint(),
                                             versions.Get(i).version()));
    }
    std::map<AgentAddr, AgentInfo*>::iterator it;
    for (it = agents_.begin(); it != agents_.end(); ++it) {
        boost::unordered_map<AgentAddr, int32_t>::iterator a_it =
                       agents_ver_map.find(it->first);
        AgentInfo* agent = it->second;
        if (agent->state() != kAlive) {
            if (a_it != agents_ver_map.end()) {
                deleted_agents->Add()->assign(it->first);
            }
            continue;
        }
        if (a_it != agents_ver_map.end() && a_it->second == it->second->version()) {
            continue;
        }
        if (a_it != agents_ver_map.end()) {
            LOG(INFO, "agent %s version has diff , the old is %d, the new is %d",
                it->second->endpoint().c_str(), a_it->second, it->second->version());
        }
        agents_info->Add()->CopyFrom(*agent);
    }
    long used_time = ::baidu::common::timer::get_micros() / 1000 - now_time;
    LOG(INFO, "process diff with time consumed %ld, agents count %d ", 
               used_time,
               agents_info->size());
    done->Run();
}
void JobManager::GetJobsOverview(JobOverviewList* jobs_overview) {
    MutexLock lock(&mutex_);
    std::map<JobId, Job*>::iterator job_it = jobs_.begin();
    for (; job_it != jobs_.end(); ++job_it) {
        const JobId& jobid = job_it->first;
        Job* job = job_it->second;
        JobOverview* overview = jobs_overview->Add();
        overview->mutable_desc()->CopyFrom(job->desc_);
        overview->mutable_desc()->mutable_pod()->Clear();
        overview->set_jobid(jobid);
        overview->set_state(job->state_);

        uint32_t running_num = 0;
        uint32_t pending_num = 0;
        uint32_t deploying_num = 0;
        std::map<PodId, PodStatus*>& pods = job->pods_;
        std::map<PodId, PodStatus*>::iterator pod_it = pods.begin();
        for (; pod_it != pods.end(); ++pod_it) {
            // const PodId& podid = pod_it->first;
            const PodStatus* pod = pod_it->second;
            if (pod->state() == kPodRunning) {
                running_num++;
                MasterUtil::AddResource(pod->resource_used(), overview->mutable_resource_used());
            } else if(pod->state() == kPodPending) {
                pending_num++;
            } else if(pod->state() == kPodDeploying) {
                deploying_num++;
            } 
        }
        overview->set_running_num(running_num);
        overview->set_pending_num(pending_num);
        overview->set_deploying_num(deploying_num);
    }
}

Status JobManager::GetJobInfo(const JobId& jobid, JobInfo* job_info) {
    MutexLock lock(&mutex_);
    std::map<JobId, Job*>::iterator job_it = jobs_.find(jobid);
    if (job_it == jobs_.end()) {
        LOG(WARNING, "get job info failed, no such job: %s", jobid.c_str());
        return kJobNotFound;
    }

    Job* job = job_it->second;
    job_info->set_jobid(jobid);
    job_info->set_state(job->state_);
    job_info->mutable_desc()->CopyFrom(job->desc_);
    std::map<PodId, PodStatus*>::iterator pod_it = job->pods_.begin();
    for (; pod_it != job->pods_.end(); ++pod_it) {
        PodStatus* pod = pod_it->second;
        job_info->add_pods()->CopyFrom(*pod);
    }
    return kOk;
}

void JobManager::BuildPodFsm() {
    fsm_.insert(std::make_pair(BuildHandlerKey(kStagePending, kStageRemoved), 
          boost::bind(&JobManager::HandleCleanPod, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStagePending, kStageRunning), 
          boost::bind(&JobManager::HandlePendingToRunning, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRunning, kStageDeath), 
          boost::bind(&JobManager::HandleRunningToDeath, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRunning, kStageRemoved), 
          boost::bind(&JobManager::HandleRunningToRemoved, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageDeath, kStageRemoved), 
          boost::bind(&JobManager::HandleRunningToRemoved, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRunning, kStageRunning), 
          boost::bind(&JobManager::HandleDoNothing, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageDeath, kStageDeath), 
          boost::bind(&JobManager::HandleRunningToDeath, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageDeath, kStagePending), 
          boost::bind(&JobManager::HandleDeathToPending, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRemoved, kStageDeath), 
          boost::bind(&JobManager::HandleCleanPod, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRemoved, kStagePending), 
          boost::bind(&JobManager::HandleCleanPod, this, _1, _2)));
    fsm_.insert(std::make_pair(BuildHandlerKey(kStageRemoved, kStageRemoved), 
          boost::bind(&JobManager::HandleDoNothing, this, _1, _2)));

}

std::string JobManager::BuildHandlerKey(const PodStage& from,
                                        const PodStage& to) {
    return PodStage_Name(from) + ":" + PodStage_Name(to);
}

void JobManager::ChangeStage(const PodStage& to,
                             PodStatus* pod,
                             Job* job) {
    mutex_.AssertHeld();
    if (safe_mode_) { 
        LOG(WARNING, "master does not support stage change in safe mode");
        return;
    }
    if (pod == NULL) {
        LOG(WARNING, "change pod invalidate input");
        return;
    }
    PodStage old_stage = pod->stage();
    std::string handler_key = BuildHandlerKey(pod->stage(), to);
    std::map<std::string, Handle>::iterator h_it = fsm_.find(handler_key);
    if (h_it == fsm_.end()) {
        LOG(WARNING, "pod %s can not change stage from %s to %s",
        pod->podid().c_str(),
        PodStage_Name(pod->stage()).c_str(),
        PodStage_Name(to).c_str());
        return;
    }
    LOG(INFO, "pod %s change stage from %s to %s",
      pod->podid().c_str(),
      PodStage_Name(old_stage).c_str(),
      PodStage_Name(to).c_str());
    h_it->second(pod, job);
}

bool JobManager::HandleCleanPod(PodStatus* pod, Job* job) {
    mutex_.AssertHeld();
    if (pod == NULL || job == NULL) {
        LOG(WARNING, "fsm the input is invalidate");
        return false;
    }
    std::map<AgentAddr, PodMap>::iterator a_it = pods_on_agent_.find(pod->endpoint());
    if (a_it != pods_on_agent_.end()) {
        PodMap::iterator p_it = a_it->second.find(pod->jobid());
        if (p_it != a_it->second.end()) {
            p_it->second.erase(pod->podid());
        }
    }
    fsm_.erase(pod->podid());
    job->pods_.erase(pod->podid());
    delete pod;
    if (job->state_ != kJobTerminated) {
        FillPodsToJob(job);
    }
    return true;
}

bool JobManager::HandlePendingToRunning(PodStatus* pod, Job* job) {
    mutex_.AssertHeld();
    if (pod == NULL || job == NULL) {
        LOG(WARNING, "fsm the input is invalidate");
        return false;
    }
    std::map<Version, PodDescriptor>::iterator it = job->pod_desc_.find(pod->version());
    if (it == job->pod_desc_.end()) {
        LOG(WARNING, "fail to find pod %d description with version %s", pod->podid().c_str(), pod->version().c_str());
        return false;
    }
    pod->set_stage(kStageRunning);
    pod->set_state(kPodDeploying); 
    RunPod(it->second, pod);
    return true;
}

bool JobManager::HandleRunningToDeath(PodStatus* pod, Job*) { 
    mutex_.AssertHeld();
    if (pod == NULL) {
        LOG(WARNING, "fsm the input is invalidate");
        return false;
    }
    pod->set_stage(kStageDeath);
    SendKillToAgent(pod);
    return true;
}

bool JobManager::HandleDeathToPending(PodStatus* pod, Job* job) {
    mutex_.AssertHeld();
    if (pod == NULL || job == NULL) {
        LOG(WARNING, "fsm the input is invalidate");
        return false;
    }
    pod->set_stage(kStagePending);
    pod->set_state(kPodPending);
    LOG(INFO, "reschedule pod %s of job %s", pod->podid().c_str(), job->id_.c_str());
    std::map<AgentAddr, PodMap>::iterator a_it = pods_on_agent_.find(pod->endpoint());
    if (a_it != pods_on_agent_.end()) {
        PodMap::iterator p_it = a_it->second.find(pod->jobid());
        if (p_it != a_it->second.end()) {
            p_it->second.erase(pod->podid());
        }
    }
    scale_up_jobs_.insert(job->id_);
    pod_cv_.Signal();
    return true;
}

bool JobManager::HandleRunningToRemoved(PodStatus* pod, Job*) {
    mutex_.AssertHeld();
    if (pod == NULL) {
        LOG(WARNING, "fsm the input is invalidate");
        return false;
    }
    pod->set_stage(kStageRemoved);
    SendKillToAgent(pod);
    return true;
}


bool JobManager::HandleDoNothing(PodStatus*, Job*) {
    return true;
}

void JobManager::SendKillToAgent(PodStatus*  pod) {
    mutex_.AssertHeld();
    if (pod == NULL) {
        return;
    }
    std::map<AgentAddr, AgentInfo*>::iterator a_it =
      agents_.find(pod->endpoint());
    if (a_it == agents_.end()) {
        LOG(WARNING, "no such agent %s", pod->endpoint().c_str());
        return;
    }
    AgentInfo* agent_info = agents_[pod->endpoint()]; 
    if (agent_info->state() == kDead) {
        return;
    }
    // tell agent to clean pod
    KillPodRequest* request = new KillPodRequest;
    KillPodResponse* response = new KillPodResponse;
    request->set_podid(pod->podid());
    Agent_Stub* stub;
    rpc_client_.GetStub(pod->endpoint(), &stub);
    boost::function<void (const KillPodRequest*, KillPodResponse*, bool, int)> kill_pod_callback;
    kill_pod_callback = boost::bind(&JobManager::KillPodCallback, this, pod->podid(),
                                    pod->jobid(),pod->endpoint(),
                                    _1, _2, _3, _4);
    rpc_client_.AsyncRequest(stub, &Agent_Stub::KillPod, request, response,
                                 kill_pod_callback, FLAGS_master_agent_rpc_timeout, 0);
    delete stub;
}

bool JobManager::SaveToNexus(const Job* job) {
    if (job == NULL) {
        return false;
    }
    JobInfo job_info;
    job_info.set_jobid(job->id_);
    job_info.set_state(job->state_);
    job_info.set_update_state(job->update_state_);
    job_info.mutable_desc()->CopyFrom(job->desc_);
    // delete pod 
    if (job_info.desc().has_pod()) { 
        job_info.mutable_desc()->release_pod();
    }
    job_info.set_latest_version(job->latest_version);
    std::map<Version, PodDescriptor>::const_iterator it = job->pod_desc_.begin();
    for(; it != job->pod_desc_.end(); ++it) {
        PodDescriptor* pod_desc = job_info.add_pod_descs();
        pod_desc->CopyFrom(it->second);
    }

    std::string job_raw_data;
    std::string job_key = FLAGS_nexus_root_path + FLAGS_jobs_store_path 
                          + "/" + job->id_;
    job_info.SerializeToString(&job_raw_data);
    ::galaxy::ins::sdk::SDKError err;
    bool put_ok = nexus_->Put(job_key, job_raw_data, &err);
    if (!put_ok) {
        LOG(WARNING, "fail to put job[%s] %s to nexus err msg %s", 
          job_info.desc().name().c_str(),
          job_info.jobid().c_str(),
          ::galaxy::ins::sdk::InsSDK::StatusToString(err).c_str());

    }
    return put_ok; 
}

bool JobManager::DeleteFromNexus(const JobId& job_id) {
    std::string job_key = FLAGS_nexus_root_path + FLAGS_jobs_store_path 
                          + "/" + job_id;
    ::galaxy::ins::sdk::SDKError err;
    bool delete_ok = nexus_->Delete(job_key, &err);
    if (!delete_ok) {
        LOG(WARNING, "fail to delete job %s from nexus err msg %s", 
          job_id.c_str(),
          ::galaxy::ins::sdk::InsSDK::StatusToString(err).c_str());
    }
    return delete_ok;
}

bool JobManager::SaveLabelToNexus(const LabelCell& label_cell) {
    std::string label_key = FLAGS_nexus_root_path + FLAGS_labels_store_path 
                            + "/" + label_cell.label();
    std::string label_value;
    if (label_cell.SerializeToString(&label_value)) {
        LOG(WARNING, "label %s serialize failed", 
                label_cell.label().c_str()); 
        return false;
    }
    ::galaxy::ins::sdk::SDKError err;
    bool put_ok = nexus_->Put(label_key, label_value, &err);    
    if (!put_ok) {
        LOG(WARNING, "fail save label %s to nexus for %s",
          label_cell.label().c_str(),
          ::galaxy::ins::sdk::InsSDK::StatusToString(err).c_str());
    }
    return put_ok;
}

}
}
