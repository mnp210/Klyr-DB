#include "klyrdb/backup/backup_manifest.hpp"
#include "klyrdb/common/checksum.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <array>
#include <algorithm>

namespace klyrdb {

const BackupComponent* BackupManifest::find_component(std::string_view name) const {
    for (const auto& comp : components) {
        if (comp.name == name) {
            return &comp;
        }
    }
    return nullptr;
}

std::string BackupManifest::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"backup_format_version\": " << backup_format_version << ",\n";
    ss << "  \"storage_format_version\": " << storage_format_version << ",\n";
    ss << "  \"backup_type\": \"" << backup_type << "\",\n";
    ss << "  \"source_db_name\": \"" << source_db_name << "\",\n";
    ss << "  \"created_at_utc\": \"" << created_at_utc << "\",\n";
    ss << "  \"created_at_epoch\": " << created_at_epoch << ",\n";
    ss << "  \"page_size\": " << page_size << ",\n";
    ss << "  \"page_count\": " << page_count << ",\n";
    ss << "  \"checkpoint_lsn\": " << checkpoint_lsn << ",\n";
    ss << "  \"min_required_lsn\": " << min_required_lsn << ",\n";
    ss << "  \"status\": \"" << status << "\",\n";
    ss << "  \"checksum_algo\": \"" << checksum_algo << "\",\n";
    ss << "  \"components\": [\n";

    for (size_t i = 0; i < components.size(); ++i) {
        const auto& c = components[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << c.name << "\",\n";
        ss << "      \"filename\": \"" << c.filename << "\",\n";
        ss << "      \"size_bytes\": " << c.size_bytes << ",\n";
        ss << "      \"crc32\": " << c.crc32 << "\n";
        ss << "    }" << (i + 1 < components.size() ? "," : "") << "\n";
    }

    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

namespace {

std::string trim(std::string_view sv) {
    size_t first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    size_t last = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(first, last - first + 1));
}

std::optional<std::string> extract_string_field(std::string_view json, std::string_view key) {
    std::string pattern = "\"" + std::string(key) + "\"";
    size_t kpos = json.find(pattern);
    if (kpos == std::string_view::npos) return std::nullopt;

    size_t colon = json.find(':', kpos + pattern.size());
    if (colon == std::string_view::npos) return std::nullopt;

    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string_view::npos) return std::nullopt;

    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return std::nullopt;

    return std::string(json.substr(q1 + 1, q2 - q1 - 1));
}

std::optional<uint64_t> extract_uint64_field(std::string_view json, std::string_view key) {
    std::string pattern = "\"" + std::string(key) + "\"";
    size_t kpos = json.find(pattern);
    if (kpos == std::string_view::npos) return std::nullopt;

    size_t colon = json.find(':', kpos + pattern.size());
    if (colon == std::string_view::npos) return std::nullopt;

    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
        start++;
    }

    size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])))) {
        end++;
    }

    if (start == end) return std::nullopt;
    try {
        return std::stoull(std::string(json.substr(start, end - start)));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

Result<BackupManifest> BackupManifest::from_json(std::string_view json_str) {
    BackupManifest m;

    auto v_opt = extract_uint64_field(json_str, "backup_format_version");
    if (!v_opt.has_value()) {
        return Status::corrupt_backup("Manifest missing or invalid 'backup_format_version'");
    }
    m.backup_format_version = static_cast<uint32_t>(*v_opt);

    auto sv_opt = extract_uint64_field(json_str, "storage_format_version");
    if (!sv_opt.has_value()) {
        return Status::corrupt_backup("Manifest missing or invalid 'storage_format_version'");
    }
    m.storage_format_version = static_cast<uint16_t>(*sv_opt);

    auto btype = extract_string_field(json_str, "backup_type");
    if (!btype.has_value()) return Status::corrupt_backup("Manifest missing 'backup_type'");
    m.backup_type = std::move(*btype);

    auto sdb = extract_string_field(json_str, "source_db_name");
    if (sdb.has_value()) m.source_db_name = std::move(*sdb);

    auto utc = extract_string_field(json_str, "created_at_utc");
    if (utc.has_value()) m.created_at_utc = std::move(*utc);

    auto ep = extract_uint64_field(json_str, "created_at_epoch");
    if (ep.has_value()) m.created_at_epoch = *ep;

    auto ps = extract_uint64_field(json_str, "page_size");
    if (ps.has_value()) m.page_size = static_cast<uint32_t>(*ps);

    auto pc = extract_uint64_field(json_str, "page_count");
    if (pc.has_value()) m.page_count = static_cast<uint32_t>(*pc);

    auto cl = extract_uint64_field(json_str, "checkpoint_lsn");
    if (cl.has_value()) m.checkpoint_lsn = *cl;

    auto ml = extract_uint64_field(json_str, "min_required_lsn");
    if (ml.has_value()) m.min_required_lsn = *ml;

    auto st = extract_string_field(json_str, "status");
    if (!st.has_value()) return Status::corrupt_backup("Manifest missing 'status'");
    m.status = std::move(*st);

    auto algo = extract_string_field(json_str, "checksum_algo");
    if (algo.has_value()) m.checksum_algo = std::move(*algo);

    // Parse components array
    size_t comps_pos = json_str.find("\"components\"");
    if (comps_pos != std::string_view::npos) {
        size_t arr_start = json_str.find('[', comps_pos);
        size_t arr_end = json_str.find(']', arr_start);
        if (arr_start != std::string_view::npos && arr_end != std::string_view::npos) {
            std::string_view arr_str = json_str.substr(arr_start, arr_end - arr_start + 1);

            size_t obj_start = 0;
            while ((obj_start = arr_str.find('{', obj_start)) != std::string_view::npos) {
                size_t obj_end = arr_str.find('}', obj_start);
                if (obj_end == std::string_view::npos) break;

                std::string_view obj_str = arr_str.substr(obj_start, obj_end - obj_start + 1);
                BackupComponent comp;

                auto c_name = extract_string_field(obj_str, "name");
                auto c_fn = extract_string_field(obj_str, "filename");
                auto c_sz = extract_uint64_field(obj_str, "size_bytes");
                auto c_crc = extract_uint64_field(obj_str, "crc32");

                if (c_name && c_fn && c_sz && c_crc) {
                    comp.name = std::move(*c_name);
                    comp.filename = std::move(*c_fn);
                    comp.size_bytes = *c_sz;
                    comp.crc32 = static_cast<uint32_t>(*c_crc);
                    m.components.push_back(std::move(comp));
                }

                obj_start = obj_end + 1;
            }
        }
    }

    return m;
}

Status BackupManifest::write_to_file(const std::filesystem::path& path) const {
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        return Status::io_error("Failed to open manifest file for writing: " + path.string());
    }
    ofs << to_json();
    ofs.flush();
    if (ofs.fail()) {
        return Status::io_error("Failed to write manifest contents: " + path.string());
    }
    return Status::ok();
}

Result<BackupManifest> BackupManifest::read_from_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Status::file_not_found("Manifest file not found: " + path.string());
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return from_json(content);
}

Result<uint32_t> calculate_file_crc32(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Status::file_not_found("File not found for checksum calculation: " + path.string());
    }

    std::array<uint8_t, 65536> buffer;
    bool has_data = false;

    // Use a continuous CRC accumulation over chunks
    // CRC-32 (IEEE 802.3): calculate_crc32 operates with XOR init 0xFFFFFFFF and final XOR 0xFFFFFFFF
    uint32_t crc_reg = 0xFFFFFFFFU;

    // Lookup table
    static constexpr auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                if (c & 1) {
                    c = (c >> 1) ^ 0xEDB88320U;
                } else {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();

    while (ifs.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || ifs.gcount() > 0) {
        has_data = true;
        const size_t bytes_read = static_cast<size_t>(ifs.gcount());
        for (size_t i = 0; i < bytes_read; ++i) {
            const uint8_t byte = buffer[i];
            const uint8_t index = static_cast<uint8_t>((crc_reg ^ byte) & 0xFF);
            crc_reg = (crc_reg >> 8) ^ table[index];
        }
    }

    if (!has_data) {
        return 0; // Empty file
    }

    return ~crc_reg;
}

std::string current_utc_timestamp_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

uint64_t current_epoch_seconds() {
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()
    );
}

} // namespace klyrdb
