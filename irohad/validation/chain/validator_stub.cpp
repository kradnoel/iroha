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

#include <validation/chain/validator_stub.hpp>

namespace iroha {
  namespace validation {

    ametsuchi::MutableStorage& ChainValidatorStub::validate(
        rxcpp::observable<dao::Block>& blocks,
        ametsuchi::MutableStorage& storage) {
      auto apply_block = [](const auto& block, auto& executor, auto& query) {
        for (const auto& tx : block.transactions) {
          for (const auto& command : tx.commands) {
            if (!executor.execute(*command)) {
              return false;
            }
          }
        }
        return true;
      };
      blocks
          .take_while(std::bind(&BlockValidator::validate, &block_validator_,
                                std::placeholders::_1))
          .subscribe([&storage, apply_block](dao::Block block) {
            storage.apply(block, apply_block);
          });
      return storage;
    }

    ChainValidatorStub::ChainValidatorStub(BlockValidator& block_validator)
        : block_validator_(block_validator) {}
  }  // namespace validation
}  // namespace iroha