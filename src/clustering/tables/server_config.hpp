// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_TABLES_SERVER_CONFIG_HPP_
#define CLUSTERING_ADMINISTRATION_TABLES_SERVER_CONFIG_HPP_

#include <memory>
#include <string>
#include <vector>

#include "rdb_protocol/artificial_table/backend.hpp"

struct fdb_node_id;
struct node_info;
struct reqlfdb_clock;

// Used to dedup logic in server_status.
struct node_name_and_id {
    std::string id_string;
    std::string name;
};
node_name_and_id compute_node_name_and_id(const fdb_node_id &node_id);

std::vector<std::pair<fdb_node_id, node_info>> read_all_node_infos(const signal_t *interruptor, FDBTransaction *txn,
                                                                   reqlfdb_clock *clock_out);

class server_config_artificial_table_fdb_backend_t :
    public artificial_table_fdb_backend_t
{
public:
    server_config_artificial_table_fdb_backend_t();
    ~server_config_artificial_table_fdb_backend_t();

    bool read_all_rows_as_vector(
            FDBDatabase *fdb,
            auth::user_context_t const &user_context,
            const signal_t *interruptor_on_caller,
            std::vector<ql::datum_t> *rows_out,
            admin_err_t *error_out) override;

    bool read_row(
            FDBTransaction *txn,
            auth::user_context_t const &user_context,
            ql::datum_t primary_key,
            const signal_t *interruptor_on_caller,
            ql::datum_t *row_out,
            admin_err_t *error_out) override;

    bool write_row(
            FDBTransaction *txn,
            auth::user_context_t const &user_context,
            ql::datum_t primary_key,
            bool pkey_was_autogenerated,
            ql::datum_t *new_value_inout,
            const signal_t *interruptor_on_caller,
            admin_err_t *error_out) override;

};

#endif /* CLUSTERING_ADMINISTRATION_TABLES_SERVER_CONFIG_HPP_ */
