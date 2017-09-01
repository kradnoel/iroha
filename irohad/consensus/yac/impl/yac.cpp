/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utility>

#include "consensus/yac/yac.hpp"
#include "consensus/yac/storage/yac_common.hpp"

namespace iroha {
  namespace consensus {
    namespace yac {

      std::shared_ptr<Yac> Yac::create(
          YacVoteStorage vote_storage,
          std::shared_ptr<YacNetwork> network,
          std::shared_ptr<YacCryptoProvider> crypto,
          std::shared_ptr<Timer> timer,
          ClusterOrdering order,
          uint64_t delay) {
        return std::make_shared<Yac>(vote_storage,
                                     network,
                                     crypto,
                                     timer,
                                     order,
                                     delay);
      }

      Yac::Yac(YacVoteStorage vote_storage,
               std::shared_ptr<YacNetwork> network,
               std::shared_ptr<YacCryptoProvider> crypto,
               std::shared_ptr<Timer> timer,
               ClusterOrdering order,
               uint64_t delay)
          : vote_storage_(std::move(vote_storage)),
            network_(std::move(network)),
            crypto_(std::move(crypto)),
            timer_(std::move(timer)),
            cluster_order_(order),
            delay_(delay) {}

      // ------|Hash gate|------

      void Yac::vote(YacHash hash, ClusterOrdering order) {
        this->cluster_order_ = order;
        votingStep(hash);
      };

      rxcpp::observable<CommitMessage> Yac::on_commit() {
        return this->notifier_.get_observable();
      }

      // ------|Network notifications|------

      void Yac::on_vote(model::Peer from, VoteMessage vote) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (crypto_->verify(vote)) {
          this->applyVote(from, vote);
        }
      }

      void Yac::on_commit(model::Peer from, CommitMessage commit) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (crypto_->verify(commit)) {
          this->applyCommit(from, commit);
        }
      }

      void Yac::on_reject(model::Peer from, RejectMessage reject) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (crypto_->verify(reject)) {
          this->applyReject(from, reject);
        }
      }

      // ------|Private interface|------

      void Yac::votingStep(YacHash hash) {
        auto committed = vote_storage_.isHashCommitted(hash.proposal_hash);
        if (committed) {
          return;
        }
        network_->send_vote(cluster_order_.currentLeader(),
                            crypto_->getVote(hash));
        timer_->invokeAfterDelay(delay_, [this, hash]() {
          cluster_order_.switchToNext();
          if (cluster_order_.hasNext()) {
            this->votingStep(hash);
          }
        });
      }

      void Yac::closeRound() {
        timer_->deny();
      };

      // ------|Apply data|------

      void Yac::applyCommit(model::Peer from, CommitMessage commit) {
        auto answer =
            vote_storage_.store(commit, cluster_order_.getNumberOfPeers());
        if (not answer.has_value()) {
          // commit don't applied
          return;
        }

        auto proposal_hash = getProposalHash(commit.votes).value();
        auto already_processed =
            vote_storage_.getProcessingState(proposal_hash);

        if (not already_processed) {

          if (answer->commit.has_value()) {
            notifier_.get_subscriber().on_next(*answer->commit);
          }

          if (answer->reject.has_value()) {
            // todo work on reject case
          }
          vote_storage_.markAsProcessedState(proposal_hash);
        }
        closeRound();
      };

      void Yac::applyReject(model::Peer from, RejectMessage reject) {
        // todo apply to vote storage
        closeRound();
      };

      void Yac::applyVote(model::Peer from, VoteMessage vote) {
        auto answer =
            vote_storage_.store(vote, cluster_order_.getNumberOfPeers());

        if (not answer.has_value()) {
          // commit don't applied
          return;
        }

        auto proposal_hash = vote.hash.proposal_hash;
        auto already_processed =
            vote_storage_.getProcessingState(proposal_hash);

        if (answer->commit.has_value()) {
          if (not already_processed) {
            // propagate for all
            propagateCommit(answer->commit.value());
          } else {
            // propagate directly
            propagateCommitDirectly(from, answer->commit.value());
          }
        }

        if (answer->reject.has_value()) {
          if (not already_processed) {
            // propagate reject for all
            propagateReject(answer->reject.value());
          } else {
            // propagate directly
            propagateRejectDirectly(from, answer->reject.value());
          }
        }

      };

      // ------|Propagation|------

      void Yac::propagateCommit(CommitMessage msg) {
        for (auto peer :cluster_order_.getPeers()) {
          propagateCommitDirectly(peer, msg);
        }
      }

      void Yac::propagateCommitDirectly(model::Peer to, CommitMessage msg) {
        network_->send_commit(std::move(to), std::move(msg));
      }

      void Yac::propagateReject(RejectMessage msg) {
        for (auto peer :cluster_order_.getPeers()) {
          propagateRejectDirectly(peer, msg);
        }
      }

      void Yac::propagateRejectDirectly(model::Peer to, RejectMessage msg) {
        network_->send_reject(std::move(to), std::move(msg));
      }

    }  // namespace yac
  }    // namespace consensus
}  // namespace iroha
