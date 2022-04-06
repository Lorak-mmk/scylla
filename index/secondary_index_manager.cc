/*
 */

/*
 * Copyright (C) 2017-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include <seastar/core/shared_ptr.hh>

#include "index/secondary_index_manager.hh"

#include "cql3/statements/index_target.hh"
#include "cql3/util.hh"
#include "cql3/expr/expression.hh"
#include "index/target_parser.hh"
#include "db/query_context.hh"
#include "schema_builder.hh"
#include "replica/database.hh"
#include "db/view/view.hh"

#include <boost/range/adaptor/map.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <stdexcept>

namespace secondary_index {

index::index(const sstring& target_column, const index_metadata& im)
    : _im{im}
    , _target_type{cql3::statements::index_target::from_target_string(target_column)}
    , _target_column{cql3::statements::index_target::column_name_from_target_string(target_column)}
{}

bool index::depends_on(const column_definition& cdef) const {
    return cdef.name_as_text() == _target_column;
}

bool index::supports_expression(const column_definition& cdef, const cql3::expr::oper_t op) const {
    using target_type = cql3::statements::index_target::target_type;
    if (cdef.name_as_text() != _target_column) {
        return false;
    }

    if (op == cql3::expr::oper_t::EQ) {
        // if (is_subscript) {
        //     return _target_type == target_type::keys_and_values;
        // }

        return _target_type == target_type::regular_values;
    }
    // if (is_subscript) {
    //     return false;
    // }
    if (op == cql3::expr::oper_t::CONTAINS && _target_type == target_type::collection_values) {
        return true;
    }
    if (op == cql3::expr::oper_t::CONTAINS_KEY && _target_type == target_type::keys) {
        return true;
    }
    return false;
}

const index_metadata& index::metadata() const {
    return _im;
}

secondary_index_manager::secondary_index_manager(data_dictionary::table cf)
    : _cf{cf}
{}

void secondary_index_manager::reload() {
    const auto& table_indices = _cf.schema()->all_indices();
    auto it = _indices.begin();
    while (it != _indices.end()) {
        auto index_name = it->first;
        if (!table_indices.contains(index_name)) {
            it = _indices.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& index : _cf.schema()->all_indices()) {
        add_index(index.second);
    }
}

void secondary_index_manager::add_index(const index_metadata& im) {
    sstring index_target = im.options().at(cql3::statements::index_target::target_option_name);
    sstring index_target_name = target_parser::get_target_column_name_from_string(index_target);
    _indices.emplace(im.name(), index{index_target_name, im});
}

sstring index_table_name(const sstring& index_name) {
    return format("{}_index", index_name);
}

sstring index_name_from_table_name(const sstring& table_name) {
    if (table_name.size() < 7 || !boost::algorithm::ends_with(table_name, "_index")) {
        throw std::runtime_error(format("Table {} does not have _index suffix", table_name));
    }
    return table_name.substr(0, table_name.size() - 6); // remove the _index suffix from an index name;
}

static bytes get_available_column_name(const schema& schema, const bytes& root) {
    bytes accepted_name = root;
    int i = 0;
    while (schema.get_column_definition(accepted_name)) {
        accepted_name = root + to_bytes("_") + to_bytes(std::to_string(++i));
    }
    return accepted_name;
}

static bytes get_available_token_column_name(const schema& schema) {
    return get_available_column_name(schema, "idx_token");
}

static bytes get_available_computed_collection_column_name(const schema& schema) {
    return get_available_column_name(schema, "coll_value");
}

static data_type type_for_computed_column(cql3::statements::index_target::target_type target, const abstract_type& collection_type) {
    using namespace cql3::statements;
    switch (target) {
        case index_target::target_type::keys:               return collection_keys_type(collection_type);
        case index_target::target_type::keys_and_values:    return collection_entries_type(collection_type);
        case index_target::target_type::collection_values:  return collection_values_type(collection_type);
        default: throw std::runtime_error("reached regular values or full when only collection index target types were expected");
    }
}

view_ptr secondary_index_manager::create_view_for_index(const index_metadata& im, bool new_token_column_computation) const {
    auto schema = _cf.schema();
    sstring index_target_name = im.options().at(cql3::statements::index_target::target_option_name);
    schema_builder builder{schema->ks_name(), index_table_name(im.name())};
    auto target_info = target_parser::parse(schema, im);
    const auto* index_target = im.local() ? target_info.ck_columns.front() : target_info.pk_columns.front();
    auto target_type = target_info.type;

    // For local indexing, start with base partition key
    if (im.local()) {
        if (index_target->is_partition_key()) {
            throw exceptions::invalid_request_exception("Local indexing based on partition key column is not allowed,"
                    " since whole base partition key must be used in queries anyway. Use global indexing instead.");
        }
        for (auto& col : schema->partition_key_columns()) {
            builder.with_column(col.name(), col.type, column_kind::partition_key);
        }
        builder.with_column(index_target->name(), index_target->type, column_kind::clustering_key);
    } else {
        if (target_type == cql3::statements::index_target::target_type::regular_values) {
            builder.with_column(index_target->name(), index_target->type, column_kind::partition_key);
        } else {
            bytes key_column_name = get_available_computed_collection_column_name(*schema);
            column_computation_ptr collection_column_computation_ptr = [&name = index_target->name(), target_type] {
                switch (target_type) {
                    case cql3::statements::index_target::target_type::keys:
                        return collection_column_computation::for_keys(name);
                    case cql3::statements::index_target::target_type::collection_values:
                        return collection_column_computation::for_values(name);
                    case cql3::statements::index_target::target_type::keys_and_values:
                        return collection_column_computation::for_entries(name);
                    default:
                        throw std::logic_error(format("create_view_for_index: invalid target_type, received {}", to_sstring(target_type)));
                }
            }().clone();

            data_type t = type_for_computed_column(target_type, *index_target->type);
            builder.with_computed_column(key_column_name, t, column_kind::partition_key, std::move(collection_column_computation_ptr));
        }
        // Additional token column is added to ensure token order on secondary index queries
        bytes token_column_name = get_available_token_column_name(*schema);
        if (new_token_column_computation) {
            builder.with_computed_column(token_column_name, long_type, column_kind::clustering_key, std::make_unique<token_column_computation>());
        } else {
            // FIXME(pgrabowski): this legacy code is here for backward compatibility and should be removed
            // once "supports_correct_idx_token_in_secondary_index" is supported by every node
            builder.with_computed_column(token_column_name, bytes_type, column_kind::clustering_key, std::make_unique<legacy_token_column_computation>());
        }

        for (auto& col : schema->partition_key_columns()) {
            if (col == *index_target) {
                continue;
            }
            builder.with_column(col.name(), col.type, column_kind::clustering_key);
        }

        // If two cells within the same collection share the same value but not liveness information, then
        // for the index on the values, the rows generated would share the same primary key and thus the
        // liveness information as well. Prevent that by distinguising them in the clustering key.
        if (target_type == cql3::statements::index_target::target_type::collection_values) {
            data_type t = type_for_computed_column(cql3::statements::index_target::target_type::keys, *index_target->type);
            bytes column_name = get_available_column_name(*schema, "coll_keys_for_values_index");
            builder.with_computed_column(column_name, t, column_kind::clustering_key, collection_column_computation::for_keys(index_target->name()).clone());
        }
    }

    for (auto& col : schema->clustering_key_columns()) {
        if (col == *index_target) {
            continue;
        }
        builder.with_column(col.name(), col.type, column_kind::clustering_key);
    }
    if (index_target->is_primary_key()) {
        for (auto& def : schema->regular_columns()) {
            db::view::create_virtual_column(builder, def.name(), def.type);
        }
    }
    const sstring where_clause = format("{} IS NOT NULL", index_target->name_as_cql_string());
    builder.with_view_info(*schema, false, where_clause);
    return view_ptr{builder.build()};
}

std::vector<index_metadata> secondary_index_manager::get_dependent_indices(const column_definition& cdef) const {
    return boost::copy_range<std::vector<index_metadata>>(_indices
           | boost::adaptors::map_values
           | boost::adaptors::filtered([&] (auto& index) { return index.depends_on(cdef); })
           | boost::adaptors::transformed([&] (auto& index) { return index.metadata(); }));
}

std::vector<index> secondary_index_manager::list_indexes() const {
    return boost::copy_range<std::vector<index>>(_indices | boost::adaptors::map_values);
}

bool secondary_index_manager::is_index(view_ptr view) const {
    return is_index(*view);
}

bool secondary_index_manager::is_index(const schema& s) const {
    return boost::algorithm::any_of(_indices | boost::adaptors::map_values, [&s] (const index& i) {
        return s.cf_name() == index_table_name(i.metadata().name());
    });
}

bool secondary_index_manager::is_global_index(const schema& s) const {
    return boost::algorithm::any_of(_indices | boost::adaptors::map_values, [&s] (const index& i) {
        return !i.metadata().local() && s.cf_name() == index_table_name(i.metadata().name());
    });
}

}
