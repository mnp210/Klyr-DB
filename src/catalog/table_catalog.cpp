#include "klyrdb/catalog/table_catalog.hpp"
#include "klyrdb/common/endian.hpp"
#include "klyrdb/storage/page_constants.hpp"
#include <cstring>
#include <unordered_set>

namespace klyrdb {

namespace {

constexpr size_t OFFSET_TC_TYPE    = 0;
constexpr size_t OFFSET_TC_COUNT   = 2;
constexpr size_t OFFSET_TC_NEXT    = 4;
constexpr size_t OFFSET_TC_LAST_ID = 8;
constexpr size_t OFFSET_TC_ENTRIES = 12;

} // namespace

Result<std::unique_ptr<TableCatalog>> TableCatalog::open_or_create(BufferPoolManager& bpm) {
    page_id_t cat_pid = bpm.page_file().metadata().table_catalog_page_id;

    if (cat_pid == INVALID_PAGE_ID) {
        // Allocate a new TableCatalog page
        auto guard_res = bpm.new_page_guard(cat_pid);
        if (guard_res.is_err()) {
            return guard_res.status();
        }

        auto& guard = guard_res.value();
        guard->set_page_type(PageType::TableCatalog);
        auto p = guard->mutable_payload();
        std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);

        encode_u16_be(static_cast<uint16_t>(PageType::TableCatalog), p.data() + OFFSET_TC_TYPE);
        encode_u16_be(0, p.data() + OFFSET_TC_COUNT);
        encode_u32_be(INVALID_PAGE_ID, p.data() + OFFSET_TC_NEXT);
        encode_u32_be(0, p.data() + OFFSET_TC_LAST_ID);
        guard->set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
        guard.mark_dirty();

        // Update database metadata on Page 0
        auto meta = bpm.page_file().metadata();
        meta.table_catalog_page_id = cat_pid;
        Status update_st = bpm.page_file().update_metadata(meta);
        if (!update_st.is_ok()) {
            return update_st;
        }

        auto catalog = std::make_unique<TableCatalog>(bpm, cat_pid);
        return catalog;
    }

    auto catalog = std::make_unique<TableCatalog>(bpm, cat_pid);
    Status load_st = catalog->load_from_page();
    if (!load_st.is_ok()) {
        return load_st;
    }

    return catalog;
}

Status TableCatalog::load_from_page() {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    if (guard->page_type() != PageType::TableCatalog) {
        return Status::page_corrupted("Page " + std::to_string(catalog_page_id_) +
                                      " is not a TableCatalog page");
    }

    const uint8_t* p = guard->payload().data();
    const size_t max_size = MAX_PAYLOAD_SIZE;

    const uint16_t table_count = decode_u16_be(p + OFFSET_TC_COUNT);
    last_table_id_ = decode_u32_be(p + OFFSET_TC_LAST_ID);

    tables_.clear();
    tables_.reserve(table_count);

    size_t offset = OFFSET_TC_ENTRIES;
    for (uint16_t i = 0; i < table_count; ++i) {
        if (offset + 2 > max_size) {
            return Status::page_corrupted("TableCatalog entry header exceeds page boundary");
        }

        const uint16_t entry_len = decode_u16_be(p + offset);
        if (offset + 2 + entry_len > max_size) {
            return Status::page_corrupted("TableCatalog entry exceeds page boundary");
        }

        const uint8_t* entry_ptr = p + offset + 2;
        const uint8_t* entry_end = entry_ptr + entry_len;

        TableSchema schema;
        schema.table_id = decode_u32_be(entry_ptr);
        entry_ptr += 4;

        uint16_t name_len = decode_u16_be(entry_ptr);
        entry_ptr += 2;
        if (entry_ptr + name_len > entry_end) {
            return Status::page_corrupted("Table name exceeds entry length");
        }
        schema.name = std::string(reinterpret_cast<const char*>(entry_ptr), name_len);
        entry_ptr += name_len;

        uint16_t col_count = decode_u16_be(entry_ptr);
        entry_ptr += 2;
        schema.columns.reserve(col_count);

        for (uint16_t c = 0; c < col_count; ++c) {
            uint16_t col_name_len = decode_u16_be(entry_ptr);
            entry_ptr += 2;
            if (entry_ptr + col_name_len + 1 > entry_end) {
                return Status::page_corrupted("Column definition exceeds entry length");
            }
            std::string col_name(reinterpret_cast<const char*>(entry_ptr), col_name_len);
            entry_ptr += col_name_len;
            auto col_type = static_cast<SqlType>(*entry_ptr++);
            schema.columns.push_back(ColumnDef{std::move(col_name), col_type});
        }

        uint16_t idx_count = decode_u16_be(entry_ptr);
        entry_ptr += 2;
        schema.indexes.reserve(idx_count);

        for (uint16_t idx = 0; idx < idx_count; ++idx) {
            uint16_t idx_name_len = decode_u16_be(entry_ptr);
            entry_ptr += 2;
            std::string idx_name(reinterpret_cast<const char*>(entry_ptr), idx_name_len);
            entry_ptr += idx_name_len;

            uint16_t c_name_len = decode_u16_be(entry_ptr);
            entry_ptr += 2;
            std::string c_name(reinterpret_cast<const char*>(entry_ptr), c_name_len);
            entry_ptr += c_name_len;

            uint16_t c_idx = decode_u16_be(entry_ptr);
            entry_ptr += 2;
            page_id_t r_pid = decode_u32_be(entry_ptr);
            entry_ptr += 4;
            bool is_uniq = (*entry_ptr++ != 0);

            schema.indexes.push_back(TableIndexInfo{
                std::move(idx_name),
                std::move(c_name),
                c_idx,
                r_pid,
                is_uniq
            });
        }

        if (entry_ptr + 2 <= entry_end) {
            uint16_t constr_count = decode_u16_be(entry_ptr);
            entry_ptr += 2;
            schema.constraints.reserve(constr_count);

            for (uint16_t k = 0; k < constr_count; ++k) {
                if (entry_ptr >= entry_end) break;
                auto c_type = static_cast<ConstraintType>(*entry_ptr++);

                uint16_t c_n_len = decode_u16_be(entry_ptr);
                entry_ptr += 2;
                std::string c_n(reinterpret_cast<const char*>(entry_ptr), c_n_len);
                entry_ptr += c_n_len;

                uint16_t col_n_len = decode_u16_be(entry_ptr);
                entry_ptr += 2;
                std::string col_n(reinterpret_cast<const char*>(entry_ptr), col_n_len);
                entry_ptr += col_n_len;

                uint16_t ref_t_len = decode_u16_be(entry_ptr);
                entry_ptr += 2;
                std::string ref_t(reinterpret_cast<const char*>(entry_ptr), ref_t_len);
                entry_ptr += ref_t_len;

                uint16_t ref_c_len = decode_u16_be(entry_ptr);
                entry_ptr += 2;
                std::string ref_c(reinterpret_cast<const char*>(entry_ptr), ref_c_len);
                entry_ptr += ref_c_len;

                uint16_t enf_i_len = decode_u16_be(entry_ptr);
                entry_ptr += 2;
                std::string enf_i(reinterpret_cast<const char*>(entry_ptr), enf_i_len);
                entry_ptr += enf_i_len;

                schema.constraints.push_back(TableConstraint{
                    c_type,
                    std::move(c_n),
                    std::move(col_n),
                    std::move(ref_t),
                    std::move(ref_c),
                    std::move(enf_i)
                });
            }
        }

        tables_.push_back(std::move(schema));
        offset += 2 + entry_len;
    }

    return Status::ok();
}

Status TableCatalog::persist_to_page() {
    auto guard_res = bpm_.fetch_page_guard(catalog_page_id_);
    if (guard_res.is_err()) return guard_res.status();

    auto& guard = guard_res.value();
    auto p = guard->mutable_payload();

    std::vector<uint8_t> buffer;
    buffer.reserve(MAX_PAYLOAD_SIZE);

    // Placeholder for header
    buffer.resize(OFFSET_TC_ENTRIES, 0);

    for (const auto& tbl : tables_) {
        // Serialize entry to temporary buffer
        std::vector<uint8_t> entry_buf;
        entry_buf.resize(4); // table_id
        encode_u32_be(tbl.table_id, entry_buf.data());

        // table name
        uint16_t n_len = static_cast<uint16_t>(tbl.name.size());
        uint8_t n_len_buf[2];
        encode_u16_be(n_len, n_len_buf);
        entry_buf.insert(entry_buf.end(), n_len_buf, n_len_buf + 2);
        entry_buf.insert(entry_buf.end(), tbl.name.begin(), tbl.name.end());

        // column count
        uint16_t c_count = static_cast<uint16_t>(tbl.columns.size());
        uint8_t c_count_buf[2];
        encode_u16_be(c_count, c_count_buf);
        entry_buf.insert(entry_buf.end(), c_count_buf, c_count_buf + 2);

        for (const auto& col : tbl.columns) {
            uint16_t cn_len = static_cast<uint16_t>(col.name.size());
            uint8_t cn_len_buf[2];
            encode_u16_be(cn_len, cn_len_buf);
            entry_buf.insert(entry_buf.end(), cn_len_buf, cn_len_buf + 2);
            entry_buf.insert(entry_buf.end(), col.name.begin(), col.name.end());
            entry_buf.push_back(static_cast<uint8_t>(col.type));
        }

        // index count
        uint16_t i_count = static_cast<uint16_t>(tbl.indexes.size());
        uint8_t i_count_buf[2];
        encode_u16_be(i_count, i_count_buf);
        entry_buf.insert(entry_buf.end(), i_count_buf, i_count_buf + 2);

        for (const auto& idx : tbl.indexes) {
            uint16_t in_len = static_cast<uint16_t>(idx.index_name.size());
            uint8_t in_len_buf[2];
            encode_u16_be(in_len, in_len_buf);
            entry_buf.insert(entry_buf.end(), in_len_buf, in_len_buf + 2);
            entry_buf.insert(entry_buf.end(), idx.index_name.begin(), idx.index_name.end());

            uint16_t icn_len = static_cast<uint16_t>(idx.column_name.size());
            uint8_t icn_len_buf[2];
            encode_u16_be(icn_len, icn_len_buf);
            entry_buf.insert(entry_buf.end(), icn_len_buf, icn_len_buf + 2);
            entry_buf.insert(entry_buf.end(), idx.column_name.begin(), idx.column_name.end());

            uint8_t num_buf[6];
            encode_u16_be(static_cast<uint16_t>(idx.column_index), num_buf);
            encode_u32_be(idx.root_page_id, num_buf + 2);
            entry_buf.insert(entry_buf.end(), num_buf, num_buf + 6);
            entry_buf.push_back(idx.is_unique ? 1 : 0);
        }

        // constraint count
        uint16_t k_count = static_cast<uint16_t>(tbl.constraints.size());
        uint8_t k_count_buf[2];
        encode_u16_be(k_count, k_count_buf);
        entry_buf.insert(entry_buf.end(), k_count_buf, k_count_buf + 2);

        for (const auto& c : tbl.constraints) {
            entry_buf.push_back(static_cast<uint8_t>(c.type));

            uint16_t c_n_len = static_cast<uint16_t>(c.name.size());
            uint8_t c_n_buf[2];
            encode_u16_be(c_n_len, c_n_buf);
            entry_buf.insert(entry_buf.end(), c_n_buf, c_n_buf + 2);
            entry_buf.insert(entry_buf.end(), c.name.begin(), c.name.end());

            uint16_t col_n_len = static_cast<uint16_t>(c.column_name.size());
            uint8_t col_n_buf[2];
            encode_u16_be(col_n_len, col_n_buf);
            entry_buf.insert(entry_buf.end(), col_n_buf, col_n_buf + 2);
            entry_buf.insert(entry_buf.end(), c.column_name.begin(), c.column_name.end());

            uint16_t ref_t_len = static_cast<uint16_t>(c.ref_table_name.size());
            uint8_t ref_t_buf[2];
            encode_u16_be(ref_t_len, ref_t_buf);
            entry_buf.insert(entry_buf.end(), ref_t_buf, ref_t_buf + 2);
            entry_buf.insert(entry_buf.end(), c.ref_table_name.begin(), c.ref_table_name.end());

            uint16_t ref_c_len = static_cast<uint16_t>(c.ref_column_name.size());
            uint8_t ref_c_buf[2];
            encode_u16_be(ref_c_len, ref_c_buf);
            entry_buf.insert(entry_buf.end(), ref_c_buf, ref_c_buf + 2);
            entry_buf.insert(entry_buf.end(), c.ref_column_name.begin(), c.ref_column_name.end());

            uint16_t enf_i_len = static_cast<uint16_t>(c.enforcement_index_name.size());
            uint8_t enf_i_buf[2];
            encode_u16_be(enf_i_len, enf_i_buf);
            entry_buf.insert(entry_buf.end(), enf_i_buf, enf_i_buf + 2);
            entry_buf.insert(entry_buf.end(), c.enforcement_index_name.begin(), c.enforcement_index_name.end());
        }

        if (buffer.size() + 2 + entry_buf.size() > MAX_PAYLOAD_SIZE) {
            return Status::payload_overflow("TableCatalog page capacity exceeded");
        }

        uint8_t entry_len_buf[2];
        encode_u16_be(static_cast<uint16_t>(entry_buf.size()), entry_len_buf);
        buffer.insert(buffer.end(), entry_len_buf, entry_len_buf + 2);
        buffer.insert(buffer.end(), entry_buf.begin(), entry_buf.end());
    }

    // Write header
    encode_u16_be(static_cast<uint16_t>(PageType::TableCatalog), buffer.data() + OFFSET_TC_TYPE);
    encode_u16_be(static_cast<uint16_t>(tables_.size()), buffer.data() + OFFSET_TC_COUNT);
    encode_u32_be(INVALID_PAGE_ID, buffer.data() + OFFSET_TC_NEXT);
    encode_u32_be(last_table_id_, buffer.data() + OFFSET_TC_LAST_ID);

    std::memset(p.data(), 0, MAX_PAYLOAD_SIZE);
    std::memcpy(p.data(), buffer.data(), buffer.size());
    guard->set_payload_size(static_cast<uint16_t>(MAX_PAYLOAD_SIZE));
    guard.mark_dirty();

    return Status::ok();
}

Result<TableSchema> TableCatalog::create_table(std::string_view table_name,
                                               std::vector<ColumnDef> columns,
                                               std::vector<TableConstraint> constraints) {
    Status val_st = validate_identifier(table_name, "Table name");
    if (!val_st.is_ok()) return val_st;

    std::string canonical_name = canonicalize_name(table_name);

    if (has_table(canonical_name)) {
        return Status::table_already_exists("Table '" + canonical_name + "' already exists");
    }

    if (columns.empty()) {
        return Status::invalid_argument("Table must have at least one column");
    }

    std::unordered_set<std::string> col_names;
    std::vector<ColumnDef> validated_columns;
    validated_columns.reserve(columns.size());

    for (const auto& col : columns) {
        Status col_val_st = validate_identifier(col.name, "Column name");
        if (!col_val_st.is_ok()) return col_val_st;

        std::string can_col_name = canonicalize_name(col.name);
        if (col_names.contains(can_col_name)) {
            return Status::duplicate_column("Duplicate column name: '" + can_col_name + "'");
        }
        col_names.insert(can_col_name);
        validated_columns.push_back(ColumnDef{can_col_name, col.type});
    }

    // Validate constraints
    size_t pk_count = 0;
    std::vector<TableConstraint> validated_constraints;
    validated_constraints.reserve(constraints.size());
    std::unordered_set<std::string> constraint_names;

    for (auto c : constraints) {
        std::string col_name = canonicalize_name(c.column_name);
        if (!col_names.contains(col_name)) {
            return Status::column_not_found("Column '" + col_name + "' in constraint not found in table '" +
                                            canonical_name + "'");
        }
        c.column_name = col_name;

        if (c.type == ConstraintType::PrimaryKey) {
            pk_count++;
            if (pk_count > 1) {
                return Status::constraint_violation("Multiple primary keys are not allowed on table '" +
                                                    canonical_name + "'");
            }
            if (c.name.empty()) {
                c.name = "pk_" + canonical_name + "_" + col_name;
            }
            c.enforcement_index_name = c.name;
        } else if (c.type == ConstraintType::Unique) {
            if (c.name.empty()) {
                c.name = "uk_" + canonical_name + "_" + col_name;
            }
            c.enforcement_index_name = c.name;
        } else if (c.type == ConstraintType::NotNull) {
            if (c.name.empty()) {
                c.name = "nn_" + canonical_name + "_" + col_name;
            }
        } else if (c.type == ConstraintType::ForeignKey) {
            std::string ref_t = canonicalize_name(c.ref_table_name);
            std::string ref_c = canonicalize_name(c.ref_column_name);

            if (!has_table(ref_t)) {
                return Status::table_not_found("Referenced parent table '" + ref_t + "' does not exist");
            }
            auto parent_res = get_table(ref_t);
            if (parent_res.is_err()) return parent_res.status();
            const auto& parent_tbl = parent_res.value();

            auto parent_col_idx = parent_tbl.find_column_index(ref_c);
            if (!parent_col_idx.has_value()) {
                return Status::column_not_found("Referenced column '" + ref_c + "' not found in parent table '" +
                                                ref_t + "'");
            }

            // Check parent column is PK or UNIQUE
            if (!parent_tbl.is_primary_key(ref_c) && !parent_tbl.is_unique(ref_c)) {
                return Status::constraint_violation("Foreign key references column '" + ref_c +
                                                    "' in table '" + ref_t +
                                                    "', which is neither a PRIMARY KEY nor UNIQUE");
            }

            // Check type compatibility
            SqlType child_type = SqlType::Integer;
            for (const auto& cd : validated_columns) {
                if (cd.name == col_name) {
                    child_type = cd.type;
                    break;
                }
            }
            SqlType parent_type = parent_tbl.columns[*parent_col_idx].type;
            if (child_type != parent_type) {
                return Status::type_mismatch("Foreign key type mismatch: column '" + col_name + "' (" +
                                             to_string(child_type) + ") does not match referenced column '" +
                                             ref_c + "' (" + to_string(parent_type) + ")");
            }

            c.ref_table_name = ref_t;
            c.ref_column_name = ref_c;
            if (c.name.empty()) {
                c.name = "fk_" + canonical_name + "_" + col_name + "_" + ref_t;
            }
        }

        c.name = canonicalize_name(c.name);
        if (constraint_names.contains(c.name)) {
            return Status::constraint_violation("Duplicate constraint name: '" + c.name + "'");
        }
        constraint_names.insert(c.name);
        validated_constraints.push_back(std::move(c));
    }

    TableSchema schema;
    schema.table_id = ++last_table_id_;
    schema.name = canonical_name;
    schema.columns = std::move(validated_columns);
    schema.constraints = std::move(validated_constraints);

    tables_.push_back(schema);

    Status persist_st = persist_to_page();
    if (!persist_st.is_ok()) {
        tables_.pop_back();
        --last_table_id_;
        return persist_st;
    }

    return schema;
}

Result<TableSchema> TableCatalog::get_table(std::string_view table_name) const {
    std::string lower = canonicalize_name(table_name);
    for (const auto& tbl : tables_) {
        if (tbl.name == lower) {
            return tbl;
        }
    }
    return Status::table_not_found("Table '" + std::string(table_name) + "' not found");
}

Result<TableSchema> TableCatalog::get_table(table_id_t table_id) const {
    for (const auto& tbl : tables_) {
        if (tbl.table_id == table_id) {
            return tbl;
        }
    }
    return Status::table_not_found("Table with ID " + std::to_string(table_id) + " not found");
}

bool TableCatalog::has_table(std::string_view table_name) const noexcept {
    std::string lower = canonicalize_name(table_name);
    for (const auto& tbl : tables_) {
        if (tbl.name == lower) {
            return true;
        }
    }
    return false;
}

Status TableCatalog::drop_table(std::string_view table_name) {
    std::string lower = canonicalize_name(table_name);
    for (auto it = tables_.begin(); it != tables_.end(); ++it) {
        if (it->name == lower) {
            tables_.erase(it);
            return persist_to_page();
        }
    }
    return Status::table_not_found("Table '" + std::string(table_name) + "' not found");
}

Status TableCatalog::add_index_to_table(std::string_view table_name, TableIndexInfo index_info) {
    std::string lower_tbl = canonicalize_name(table_name);
    index_info.index_name = canonicalize_name(index_info.index_name);
    index_info.column_name = canonicalize_name(index_info.column_name);

    for (auto& tbl : tables_) {
        if (tbl.name == lower_tbl) {
            if (tbl.find_index_by_name(index_info.index_name) != nullptr) {
                return Status::duplicate_key("Index '" + index_info.index_name + "' already exists");
            }
            tbl.indexes.push_back(std::move(index_info));
            return persist_to_page();
        }
    }
    return Status::table_not_found("Table '" + std::string(table_name) + "' not found");
}

Status TableCatalog::update_index_root(std::string_view table_name, std::string_view index_name, page_id_t new_root_id) {
    std::string lower_tbl = canonicalize_name(table_name);
    std::string lower_idx = canonicalize_name(index_name);

    for (auto& tbl : tables_) {
        if (tbl.name == lower_tbl) {
            for (auto& idx : tbl.indexes) {
                if (idx.index_name == lower_idx) {
                    idx.root_page_id = new_root_id;
                    return persist_to_page();
                }
            }
            return Status::index_not_found("Index '" + lower_idx + "' not found on table '" + lower_tbl + "'");
        }
    }
    return Status::table_not_found("Table '" + std::string(table_name) + "' not found");
}

Status TableCatalog::drop_index_from_table(std::string_view table_name, std::string_view index_name) {
    std::string lower_tbl = canonicalize_name(table_name);
    std::string lower_idx = canonicalize_name(index_name);

    for (auto& tbl : tables_) {
        if (tbl.name == lower_tbl) {
            for (auto it = tbl.indexes.begin(); it != tbl.indexes.end(); ++it) {
                if (it->index_name == lower_idx) {
                    tbl.indexes.erase(it);
                    return persist_to_page();
                }
            }
            return Status::index_not_found("Index '" + lower_idx + "' not found on table '" + lower_tbl + "'");
        }
    }
    return Status::table_not_found("Table '" + std::string(table_name) + "' not found");
}

std::vector<TableSchema> TableCatalog::list_tables() const {
    return tables_;
}

} // namespace klyrdb
