// Copyright (c) 2013, Cloudera, inc.

#include "kudu/consensus/local_consensus.h"

#include <boost/thread/locks.hpp>
#include <boost/assign/list_of.hpp>
#include <iostream>

#include "kudu/consensus/log.h"
#include "kudu/server/metadata.h"
#include "kudu/server/clock.h"
#include "kudu/util/trace.h"

namespace kudu {
namespace consensus {

using base::subtle::Barrier_AtomicIncrement;
using consensus::CHANGE_CONFIG_OP;
using log::Log;
using log::LogEntryBatch;
using metadata::QuorumPB;
using metadata::QuorumPeerPB;
using std::tr1::shared_ptr;

LocalConsensus::LocalConsensus(const ConsensusOptions& options)
    : options_(options),
      next_op_id_index_(-1),
      state_(kNotInitialized),
      log_(NULL) {
}

Status LocalConsensus::Init(const QuorumPeerPB& peer,
                            const scoped_refptr<server::Clock>& clock,
                            ReplicaTransactionFactory* txn_factory,
                            Log* log) {
  CHECK_EQ(state_, kNotInitialized);
  peer_ = peer;
  clock_ = clock;
  log_ = log;
  state_ = kInitializing;

  // Determine initial OpId
  // TODO: do this in Start() using the ConsensusBootstrapInfo
  OpId initial;
  Status s = log->GetLastEntryOpId(&initial);
  if (s.ok()) {
    // We are continuing after previously running.
  } else if (s.IsNotFound()) {
    // This is our very first startup! Sally forth!
    initial = log::MinimumOpId();
  } else {
    LOG(FATAL) << "Unexpected status from Log::GetLastEntryOpId(): "
               << s.ToString();
  }
  next_op_id_index_ = initial.index() + 1;

  return Status::OK();
}

Status LocalConsensus::Start(const metadata::QuorumPB& initial_quorum,
                             const ConsensusBootstrapInfo& bootstrap_info,
                             gscoped_ptr<metadata::QuorumPB>* running_quorum) {
  CHECK_EQ(state_, kInitializing);

  CHECK(initial_quorum.local()) << "Local consensus must be passed a local quorum";
  CHECK_LE(initial_quorum.peers_size(), 1);

  // Because this is the local consensus we always push the configuration,
  // in the dist. impl. we only try and push if we're leader.
  gscoped_ptr<ReplicateMsg> replicate_msg(new ReplicateMsg);
  replicate_msg->set_op_type(CHANGE_CONFIG_OP);
  ChangeConfigRequestPB* req = replicate_msg->mutable_change_config_request();

  // FIXME: Seems like a hack to get the current tablet ID from the Log.
  req->set_tablet_id(log_->tablet_id());
  QuorumPB* new_config = req->mutable_new_config();
  new_config->CopyFrom(initial_quorum);
  new_config->set_seqno(initial_quorum.seqno() + 1);

  shared_ptr<LatchCallback> replicate_clbk(new LatchCallback);
  shared_ptr<LatchCallback> commit_clbk(new LatchCallback);
  state_ = kConfiguring;

  TRACE("Replicating initial config");
  gscoped_ptr<ConsensusRound> round(NewRound(replicate_msg.Pass(),
                                               replicate_clbk,
                                               commit_clbk));
  RETURN_NOT_OK(Replicate(round.get()));
  RETURN_NOT_OK(replicate_clbk->Wait());

  TRACE("Committing local config");
  ChangeConfigResponsePB resp;
  gscoped_ptr<CommitMsg> commit_msg(new CommitMsg);
  commit_msg->set_op_type(CHANGE_CONFIG_OP);
  commit_msg->mutable_commited_op_id()->CopyFrom(round->replicate_op()->id());
  commit_msg->mutable_change_config_response()->CopyFrom(resp);
  commit_msg->set_timestamp(clock_->Now().ToUint64());
  RETURN_NOT_OK(round->Commit(commit_msg.Pass()));


  RETURN_NOT_OK(commit_clbk->Wait());
  TRACE("Consensus started");

  running_quorum->reset(req->release_new_config());

  quorum_ = initial_quorum;
  state_ = kRunning;
  return Status::OK();
}

Status LocalConsensus::Replicate(ConsensusRound* context) {
  DCHECK_GE(state_, kConfiguring);

  LogEntryBatch* reserved_entry_batch;
  OpId* cur_op_id = DCHECK_NOTNULL(context->replicate_op())->mutable_id();
  cur_op_id->set_term(0);
  const consensus::OperationPB* op = context->replicate_op();

  // Pre-cache the ByteSize outside of the lock, since this is somewhat
  // expensive.
  ignore_result(op->ByteSize());

  {
    boost::lock_guard<simple_spinlock> lock(lock_);

    // create the new op id for the entry.
    cur_op_id->set_index(next_op_id_index_++);
    // Reserve the correct slot in the log for the replication operation.
    gscoped_ptr<log::LogEntryBatchPB> entry_batch;
    log::CreateBatchFromAllocatedOperations(&op, 1, &entry_batch);

    RETURN_NOT_OK(log_->Reserve(entry_batch.Pass(), &reserved_entry_batch));
  }
  // Serialize and mark the message as ready to be appended.
  // When the Log actually fsync()s this message to disk, 'repl_callback'
  // is triggered.
  RETURN_NOT_OK(log_->AsyncAppend(reserved_entry_batch,
                                  context->replicate_callback()->AsStatusCallback()));

  return Status::OK();
}

Status LocalConsensus::Update(const ConsensusRequestPB* request,
                              ConsensusResponsePB* response) {
  return Status::NotSupported("LocalConsensus does not support Update() calls.");
}

Status LocalConsensus::RequestVote(const VoteRequestPB* request,
                                   VoteResponsePB* response) {
  return Status::NotSupported("LocalConsensus does not support RequestVote() calls.");
}

Status LocalConsensus::Commit(ConsensusRound* round) {

  OperationPB* commit_op = DCHECK_NOTNULL(round->commit_op());
  DCHECK(commit_op->has_commit()) << "A commit operation must have a commit.";

  LogEntryBatch* reserved_entry_batch;
  shared_ptr<FutureCallback> commit_clbk;

  // The commit callback is the very last thing to execute in a transaction
  // so it needs to free all resources. We need release it from the
  // ConsensusRound or we'd get a cycle. (callback would free the
  // TransactionState which would free the ConsensusRound, which in turn
  // would try to free the callback).
  round->release_commit_callback(&commit_clbk);

  // entry for the CommitMsg -- call mutable_id outside the lock
  // since this can do an allocation
  OpId* commit_id = commit_op->mutable_id();
  commit_id->set_term(0);

  // Pre-cache the ByteSize outside of the lock, since this is somewhat
  // expensive.
  ignore_result(commit_op->ByteSize());
  {
    boost::lock_guard<simple_spinlock> lock(lock_);
    commit_id->set_index(next_op_id_index_++);
    // Reserve the correct slot in the log for the commit operation.
    gscoped_ptr<log::LogEntryBatchPB> entry_batch;
    log::CreateBatchFromAllocatedOperations(&commit_op, 1, &entry_batch);

    RETURN_NOT_OK(log_->Reserve(entry_batch.Pass(), &reserved_entry_batch));
  }
  // Serialize and mark the message as ready to be appended.
  // When the Log actually fsync()s this message to disk, 'commit_clbk'
  // is triggered.
  RETURN_NOT_OK(log_->AsyncAppend(reserved_entry_batch,
                                  commit_clbk->AsStatusCallback()));
  return Status::OK();
}

void LocalConsensus::Shutdown() {
  VLOG(1) << "LocalConsensus Shutdown!";
}

void LocalConsensus::DumpStatusHtml(std::ostream& out) const {
  out << "<h1>Local Consensus Status</h1>\n";

  boost::lock_guard<simple_spinlock> lock(lock_);
  out << "next op: " << next_op_id_index_;
}

} // end namespace consensus
} // end namespace kudu