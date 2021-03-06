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

#include "ametsuchi/impl/storage_impl.hpp"
#include "ametsuchi/impl/mutable_storage_impl.hpp"
#include "ametsuchi/impl/postgres_wsv_command.hpp"
#include "ametsuchi/impl/postgres_wsv_query.hpp"
#include "ametsuchi/impl/temporary_wsv_impl.hpp"
#include "model/converters/json_common.hpp"

namespace iroha {
  namespace ametsuchi {

    StorageImpl::StorageImpl(
        std::string block_store_dir, std::string redis_host,
        std::size_t redis_port, std::string postgres_options,
        std::unique_ptr<FlatFile> block_store,
        std::unique_ptr<cpp_redis::redis_client> index,
        std::unique_ptr<pqxx::lazyconnection> wsv_connection,
        std::unique_ptr<pqxx::nontransaction> wsv_transaction,
        std::unique_ptr<WsvQuery> wsv)
        : block_store_dir_(block_store_dir),
          redis_host_(redis_host),
          redis_port_(redis_port),
          postgres_options_(postgres_options),
          block_store_(std::move(block_store)),
          index_(std::move(index)),
          wsv_connection_(std::move(wsv_connection)),
          wsv_transaction_(std::move(wsv_transaction)),
          wsv_(std::move(wsv)) {
      log_ = logger::log("StorageImpl");

      wsv_transaction_->exec(init_);
      wsv_transaction_->exec(
          "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY;");
    }

    std::unique_ptr<TemporaryWsv> StorageImpl::createTemporaryWsv() {
      // TODO lock

      auto postgres_connection =
          std::make_unique<pqxx::lazyconnection>(postgres_options_);
      try {
        postgres_connection->activate();
      } catch (const pqxx::broken_connection &e) {
        log_->error("Connection to PostgreSQL broken: {}", e.what());
        return nullptr;
      }
      auto wsv_transaction = std::make_unique<pqxx::nontransaction>(
          *postgres_connection, "TemporaryWsv");
      std::unique_ptr<WsvQuery> wsv =
          std::make_unique<PostgresWsvQuery>(*wsv_transaction);
      std::unique_ptr<WsvCommand> executor =
          std::make_unique<PostgresWsvCommand>(*wsv_transaction);

      return std::make_unique<TemporaryWsvImpl>(
          std::move(postgres_connection), std::move(wsv_transaction),
          std::move(wsv), std::move(executor));
    }

    std::unique_ptr<MutableStorage> StorageImpl::createMutableStorage() {
      // TODO lock

      auto postgres_connection =
          std::make_unique<pqxx::lazyconnection>(postgres_options_);
      try {
        postgres_connection->activate();
      } catch (const pqxx::broken_connection &e) {
        log_->error("Connection to PostgreSQL broken: {}", e.what());
        return nullptr;
      }
      auto wsv_transaction = std::make_unique<pqxx::nontransaction>(
          *postgres_connection, "TemporaryWsv");
      std::unique_ptr<WsvQuery> wsv =
          std::make_unique<PostgresWsvQuery>(*wsv_transaction);
      std::unique_ptr<WsvCommand> executor =
          std::make_unique<PostgresWsvCommand>(*wsv_transaction);

      auto index = std::make_unique<cpp_redis::redis_client>();
      try {
        index->connect(redis_host_, redis_port_);
      } catch (const cpp_redis::redis_error &e) {
        log_->error("Connection to Redis broken: {}", e.what());
        return nullptr;
      }

      hash256_t top_hash;

      if (block_store_->last_id()) {
        auto blob = block_store_->get(block_store_->last_id());
        if (not blob.has_value()) {
          log_->error("Fetching of blob failed");
          return nullptr;
        }

        auto document = model::converters::vectorToJson(blob.value());
        if (not document.has_value()) {
          log_->error("Blob parsing failed");
          return nullptr;
        }

        auto block = serializer_.deserialize(document.value());
        if (not block.has_value()) {
          log_->error("Deserialization of block failed");
          return nullptr;
        }
        top_hash = block->hash;
      } else {
        top_hash.fill(0);
      }

      return std::make_unique<MutableStorageImpl>(
          top_hash, std::move(index), std::move(postgres_connection),
          std::move(wsv_transaction), std::move(wsv), std::move(executor));
    }

    std::shared_ptr<StorageImpl> StorageImpl::create(
        std::string block_store_dir, std::string redis_host,
        std::size_t redis_port, std::string postgres_options) {
      auto log_ = logger::log("StorageImpl:create");
      log_->info("Start storage creation");
      // TODO lock

      auto block_store = FlatFile::create(block_store_dir);
      if (!block_store) {
        log_->error("Cannot create block store in {}", block_store_dir);
        return nullptr;
      }
      log_->info("block store created");

      auto index = std::make_unique<cpp_redis::redis_client>();
      try {
        index->connect(redis_host, redis_port);
      } catch (const cpp_redis::redis_error &e) {
        log_->error("Connection {}:{} with Redis broken",
                    redis_host,
                    redis_port);
        return nullptr;
      }
      log_->info("connection to Redis completed");

      auto postgres_connection =
          std::make_unique<pqxx::lazyconnection>(postgres_options);
      try {
        postgres_connection->activate();
      } catch (const pqxx::broken_connection &e) {
        log_->error("Cannot with PostgreSQL broken: {}", e.what());
        return nullptr;
      }
      log_->info("connection to PostgreSQL completed");

      auto wsv_transaction = std::make_unique<pqxx::nontransaction>(
          *postgres_connection, "Storage");
      std::unique_ptr<WsvQuery> wsv =
          std::make_unique<PostgresWsvQuery>(*wsv_transaction);
      log_->info("transaction to PostgreSQL initialized");

      return std::shared_ptr<StorageImpl>(
          new StorageImpl(block_store_dir, redis_host, redis_port,
                          postgres_options, std::move(block_store),
                          std::move(index), std::move(postgres_connection),
                          std::move(wsv_transaction), std::move(wsv)));
    }

    void StorageImpl::commit(std::unique_ptr<MutableStorage> mutableStorage) {
      std::unique_lock<std::shared_timed_mutex> write(rw_lock_);
      auto storage_ptr = std::move(mutableStorage);  // get ownership of storage
      auto storage = static_cast<MutableStorageImpl *>(storage_ptr.get());
      for (const auto &block : storage->block_store_) {
        block_store_->add(block.first,
                          model::converters::jsonToVector(
                              serializer_.serialize(block.second)));
      }
      storage->index_->exec();
      storage->transaction_->exec("COMMIT;");
      storage->committed = true;
    }

    rxcpp::observable<model::Transaction> StorageImpl::getAccountTransactions(
        std::string account_id) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      auto last_id = block_store_->last_id();
      return getBlocks(1, last_id)
          .flat_map([](auto block) {
            return rxcpp::observable<>::iterate(block.transactions);
          })
          .filter([account_id](auto tx) {
            return tx.creator_account_id == account_id;
          });
    }

    rxcpp::observable<model::Block> StorageImpl::getBlocks(uint32_t from,
                                                           uint32_t to) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      auto last_id = block_store_->last_id();
      if (to > last_id) {
        to = last_id;
      }
      return rxcpp::observable<>::range(from, to).flat_map([this](auto i) {
        auto bytes = block_store_->get(i);
        return rxcpp::observable<>::create<model::Block>(
            [this, bytes](auto s) {
              if (not bytes.has_value()) {
                s.on_completed();
              }
              auto document = model::converters::vectorToJson(bytes.value());
              if (not document.has_value()) {
                s.on_completed();
              }
              auto block = serializer_.deserialize(document.value());
              if (not block.has_value()) {
                s.on_completed();
              }
              s.on_next(block.value());
              s.on_completed();
            });
      });
    }

    nonstd::optional<model::Account> StorageImpl::getAccount(
        const std::string &account_id) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      return wsv_->getAccount(account_id);
    }

    nonstd::optional<std::vector<ed25519::pubkey_t>>
    StorageImpl::getSignatories(const std::string &account_id) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      return wsv_->getSignatories(account_id);
    }

    nonstd::optional<model::Asset> StorageImpl::getAsset(
        const std::string &asset_id) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      return wsv_->getAsset(asset_id);
    }

    nonstd::optional<model::AccountAsset> StorageImpl::getAccountAsset(
        const std::string &account_id, const std::string &asset_id) {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      return wsv_->getAccountAsset(account_id, asset_id);
    }

    nonstd::optional<std::vector<model::Peer>> StorageImpl::getPeers() {
      std::shared_lock<std::shared_timed_mutex> write(rw_lock_);
      return wsv_->getPeers();
    }

  }  // namespace ametsuchi
}  // namespace iroha
