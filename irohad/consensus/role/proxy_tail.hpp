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

#ifndef IROHA_PROXY_TAIL_HPP
#define IROHA_PROXY_TAIL_HPP

#include "validator.hpp"
namespace consensus {
  namespace role {

    class ProxyTail final : public Validator {
     public:
      ProxyTail(consensus::Consensus &consensus,
                peerservice::PeerServiceImpl &ps,
                std::atomic<bool> &round_started);
      Role self() override;
      virtual void on_proposal(const model::Proposal &proposal) override;
      virtual void on_commit(const model::Commit &commit) override;
      virtual void on_vote(const model::Vote &vote) override;
      virtual void on_abort(const model::Abort &abort) override;

     private:
      // TODO: write vote validator and put it here
    };
  }
}

#endif  // IROHA_PROXY_TAIL_HPP