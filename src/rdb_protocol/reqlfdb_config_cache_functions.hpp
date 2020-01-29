#ifndef RETHINKDB_RDB_PROTOCOL_REQLFDB_CONFIG_CACHE_FUNCTIONS_HPP_
#define RETHINKDB_RDB_PROTOCOL_REQLFDB_CONFIG_CACHE_FUNCTIONS_HPP_

#include "buffer_cache/types.hpp"
#include "containers/counted.hpp"
#include "containers/optional.hpp"
#include "containers/uuid.hpp"
#include "fdb/reql_fdb.hpp"
#include "fdb/typed.hpp"
#include "rdb_protocol/reqlfdb_config_cache.hpp"

namespace ql {
class db_t;
}

// TODO: Move this into reqlfdb_config_cache.hpp

// These functions are declared here because reqlfdb_config_cache is used by context.hpp
// which triggers a big rebuild when they change.

// Implementations are in reqlfdb_config_cache.cc.

class config_version_check_later {
public:
    // UINT64_MAX means no future, no check
    reqlfdb_config_version expected_config_version = {UINT64_MAX};
    fdb_value_fut<reqlfdb_config_version> config_version_future;
};

config_info<optional<database_id_t>>
config_cache_retrieve_db_by_name(
    const reqlfdb_config_cache *cc, FDBTransaction *txn,
    const name_string_t &db_name, const signal_t *interruptor);

config_info<optional<std::pair<namespace_id_t, table_config_t>>>
config_cache_retrieve_table_by_name(
    const reqlfdb_config_cache *cc, FDBTransaction *txn,
    const std::pair<database_id_t, name_string_t> &db_table_name,
    const signal_t *interruptor);

MUST_USE bool config_cache_db_create(
    FDBTransaction *txn,
    const name_string_t &db_name,
    const database_id_t &new_db_id,
    const signal_t *interruptor);

MUST_USE bool config_cache_table_create(
    FDBTransaction *txn,
    const namespace_id_t &new_table_id,
    const table_config_t &config,
    const signal_t *interruptor);

fdb_future transaction_get_table_range(
    FDBTransaction *txn, const database_id_t db_id,
    const std::string &lower_bound_table_name, bool closed,
    FDBStreamingMode streaming_mode);

std::string unserialize_table_by_name_table_name(key_view key, database_id_t db_id);

// Doesn't update config version!
MUST_USE bool help_remove_table_if_exists(
    FDBTransaction *txn,
    database_id_t db_id,
    const std::string &table_name,
    const signal_t *interruptor);

MUST_USE optional<std::pair<namespace_id_t, table_config_t>> config_cache_table_drop(
        FDBTransaction *txn, database_id_t db_id, const name_string_t &table_name,
        const signal_t *interruptor);

MUST_USE optional<database_id_t> config_cache_db_drop(
    FDBTransaction *txn,
    const name_string_t &db_name, const signal_t *interruptor);

// TODO: This could just return name_string_t's.
std::vector<counted_t<const ql::db_t>> config_cache_db_list(
    FDBTransaction *txn,
    const signal_t *interruptor);


std::vector<name_string_t> config_cache_table_list(
    FDBTransaction *txn,
    const database_id_t &db_id,
    const signal_t *interruptor);

#endif  // RETHINKDB_RDB_PROTOCOL_REQLFDB_CONFIG_CACHE_FUNCTIONS_HPP_