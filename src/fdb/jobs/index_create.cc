#include "fdb/jobs/index_create.hpp"

#include "clustering/administration/tables/table_metadata.hpp"
#include "fdb/index.hpp"
#include "fdb/jobs/job_utils.hpp"
#include "fdb/reql_fdb_utils.hpp"
#include "fdb/system_tables.hpp"
#include "fdb/typed.hpp"
#include "rdb_protocol/btree.hpp"  // For compute_keys

/* We encounter a question of conflict resolution when building a secondary index.  The
reason is, there is a min_pkey.  The problem is, with a naive implementation:

1. When performing a write (key W), we must read the job info for min_pkey.

2. Every sindex building operation requires that min_pkey be read, with value K1, and
written, with value K2.  It also reads all the pkey rows with values (K1, K2].

There are three key regions involved.  A. [-infinity, K1], B. (K1, K2], C. (K2, +infinity).

Our write W only knows the value K1, because K2 is TBD.

- If W is in region A, our write will need to insert into the new index.

- If W is in region C (whatever that turns out to be), then our write will not need to
  insert into the new index.

- If W is in region B, then it will conflict with the index building operation.

We can safely use a snapshot read on min_pkey, because any write in region B would
conflict with the actual key range.  But, under high write load, the conflicts are still
a problem.

What we'd like is for index building to win every conflict it has.  We want index
building to be able to make progress.  However, it's a fact of life that writes, when
they commit, will return success immediately, and they can't be "held back" to see
whether they'll end up in region B or C.

One workaround would be, if an index build fails from a conflict, to then add a "claim"
on the specific interval B that it tried to write.  Any write that would operate in a
"claim" region backs off for a specified interval of time.  However, updating the claim
region itself is something that would conflict with writes... but there are workarounds,
like writing a claim region, and then incrementing a separate version field.

*/

/* How things work:

Whenever we perform a write (to a table), we "read" the table config metadata --
discovering what sindexes there are, if there's a write hook, etc.  But we already have
it in-cache, we just check the config version.  So when building an index, we don't want
to update the table config more then twice: when index building begins, and when it
ends.  So the table config's sindex_config_t holds a shared task id, that's it, until
the index is done being built.

The information about the in-progress construction of the index changes frequently, so
it needs to be stored elsewhere.  It gets stored in index_jobstate_by_task.
*/

// TODO: To handle time series case, where index creation chases/lags behind time series
// insertion point, we should compute [start,end] key interval of index creation task
// and put that in the jobstate.  Two keys instead of one.

ql::datum_t parse_table_value(const char *value, size_t data_length) {
    buffer_read_stream_t stream(value, data_length);

    ql::datum_t ret;
    archive_result_t res = ql::datum_deserialize(&stream, &ret);
    guarantee(!bad(res), "table value misparsed");  // TODO: msg, graceful, etc.
    guarantee(size_t(stream.tell()) == data_length);  // TODO: msg, graceful, etc.
    return ret;
}

optional<fdb_job_info> execute_index_create_job(
        FDBTransaction *txn, const fdb_job_info &info,
        const fdb_job_index_create &index_create_info, const signal_t *interruptor) {
    // TODO: Maybe caller can pass clock (as in all jobs).

    fdb_value_fut<reqlfdb_clock> clock_fut = transaction_get_clock(txn);
    fdb_value_fut<fdb_job_info> real_info_fut
        = transaction_get_real_job_info(txn, info);
    fdb_value_fut<table_config_t> table_config_fut
        = transaction_lookup_uq_index<table_config_by_id>(txn, index_create_info.table_id);
    fdb_value_fut<reqlfdb_config_version> cv_fut
        = transaction_get_config_version(txn);

    // OOO: This should be a snapshot read.  Or wait, that's the write that needs a snapshot read?
    fdb_value_fut<fdb_index_jobstate> jobstate_fut
        = transaction_lookup_uq_index<index_jobstate_by_task>(txn, info.shared_task_id);

    if (!block_and_check_info(info, std::move(real_info_fut), interruptor)) {
        return r_nullopt;
    }

    // TODO: Obviously, index creation needs to initialize the jobstate when it
    // initialize the job.
    fdb_index_jobstate jobstate;
    if (!jobstate_fut.block_and_deserialize(interruptor, &jobstate)) {
        crash("fdb jobstate in invalid state");  // TODO better msg, graceful, etc.
    }

    // So we've got the key from which to start scanning.  Now what?  Scan in a big
    // block?  Small block?  Medium?  Going with medium.

    std::string pkey_prefix = table_pkey_prefix(index_create_info.table_id);

    fdb_future data_fut = transaction_uq_index_get_range(txn, pkey_prefix,
        jobstate.unindexed_lower_bound, nullptr,
        0, 0, FDB_STREAMING_MODE_MEDIUM, 0, false, false);

    // TODO: Apply a workaround for write contention problems mentioned above.
    table_config_t table_config;
    if (!table_config_fut.block_and_deserialize(interruptor, &table_config)) {
        crash("Missing table config referencing job");  // TODO: msg, graceful, etc.
    }

    const auto sindexes_it = table_config.sindexes.find(index_create_info.sindex_name);
    guarantee(sindexes_it != table_config.sindexes.end());  // TODO: msg, graceful
    // fdb_sindexes_it is (very casually) used to mutate table_config and write the
    // updated version later).
    const auto fdb_sindexes_it = table_config.fdb_sindexes.find(index_create_info.sindex_name);
    guarantee(fdb_sindexes_it != table_config.fdb_sindexes.end());  // TODO: msg, graceful

    const sindex_config_t &sindex_config = sindexes_it->second;
    {
        const sindex_metaconfig_t &sindex_metaconfig = fdb_sindexes_it->second;
        guarantee(sindex_metaconfig.sindex_id == index_create_info.sindex_id);  // TODO: msg, graceful
        guarantee(sindex_metaconfig.creation_task_or_nil == info.shared_task_id);  // TODO: msg, graceful
    }

    // LARGEVAL: Implementing large values will need handling here.

    const FDBKeyValue *kvs;
    int kv_count;
    fdb_bool_t more;
    fdb_error_t err = fdb_future_get_keyvalue_array(data_fut.fut, &kvs, &kv_count, &more);
    check_for_fdb_transaction(err);

    // TODO: Maybe FDB should store sindex_disk_info_t, using
    // sindex_reql_version_info_t.

    // TODO: Making this copy is gross -- would be better if compute_keys took sindex_config.
    sindex_disk_info_t index_info{
        sindex_config.func,
        sindex_reql_version_info_t{sindex_config.func_version,sindex_config.func_version,sindex_config.func_version},  // TODO: Verify we just dumbly use latest_compatible_reql_version.
        sindex_config.multi,
        sindex_config.geo};

    // Okay, now compute the sindex write.

    std::string index_prefix = table_index_prefix(index_create_info.table_id,
        index_create_info.sindex_id);

    // We reuse the same buffer through the loop.
    std::string fdb_key = index_prefix;

    // See rdb_update_sindexes    <- TODO: Remove this comment.
    for (int i = 0; i < kv_count; ++i) {
        key_view full_key{void_as_uint8(kvs[i].key), kvs[i].key_length};
        key_view pkey_view = full_key.guarantee_without_prefix(pkey_prefix);
        // TODO: Needless copy.
        // TODO: Increase MAX_KEY_SIZE at some point.
        store_key_t primary_key(pkey_view.length, pkey_view.data);
        ql::datum_t doc = parse_table_value(void_as_char(kvs[i].value), kvs[i].value_length);

        // TODO: The ql::datum_t value is unused.  Remove it once FDB-ized fully.
        std::vector<std::pair<store_key_t, ql::datum_t>> keys;
        compute_keys(primary_key, std::move(doc), index_info, &keys, nullptr);

        for (auto &sindex_key_pair : keys) {
            // TODO: Make sure fdb key limits are followed.
            rdbtable_sindex_fdb_key_onto(&fdb_key, sindex_key_pair.first);
            uint8_t value[1];
            fdb_transaction_set(txn,
                as_uint8(fdb_key.data()), int(fdb_key.size()),
                value, 0);
            fdb_key.resize(index_prefix.size());
        }
    }

    optional<fdb_job_info> ret;
    if (more) {
        if (kv_count > 0) {
            key_view full_key{
                void_as_uint8(kvs[kv_count - 1].key),
                kvs[kv_count - 1].key_length};
            key_view pkey_view = full_key.guarantee_without_prefix(pkey_prefix);
            std::string pkey_str{as_char(pkey_view.data), size_t(pkey_view.length)};
            // Increment the pkey lower bound since it's inclusive and we need to do
            // that.
            pkey_str.push_back('\0');
            fdb_index_jobstate new_jobstate{ukey_string{std::move(pkey_str)}};

            transaction_set_uq_index<index_jobstate_by_task>(txn, info.shared_task_id,
                new_jobstate);
        }

        reqlfdb_clock current_clock = clock_fut.block_and_deserialize(interruptor);
        fdb_job_info new_info = update_job_counter(txn, current_clock, info);
        ret.set(std::move(new_info));
    } else {
        transaction_erase_uq_index<index_jobstate_by_task>(txn, info.shared_task_id);

        remove_fdb_job(txn, info);

        // fdb_sindexes_it points into table_config.
        fdb_sindexes_it->second.creation_task_or_nil = fdb_shared_task_id{nil_uuid()};
        // Table by name index unchanged.
        transaction_set_uq_index<table_config_by_id>(txn, index_create_info.table_id,
            table_config);
        reqlfdb_config_version cv = cv_fut.block_and_deserialize(interruptor);
        cv.value++;
        transaction_set_config_version(txn, cv);
        ret = r_nullopt;
    }
    commit(txn, interruptor);
    return ret;
}

// TODO: Handle all LARGEVAL comments.