#include <iostream>
#include <fstream>
#include <string>
#include <tuple>
#include <set>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <cstdint>
#include <unordered_map>
#include <filesystem>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/small_vector.hpp>


// 各種構造体
struct Link {
    uint8_t move;
    int8_t eval_link;
    bool visited;
};

struct Leaf {
    uint8_t move;
    int8_t eval;
    bool visited;
};

// ハッシュ関数 シンプルいずベスト
struct PairHash {
    std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const {
        uint64_t combined = p.first + 0x9e3779b97f4a7c15ULL + (p.second << 12) + (p.second >> 4);
        return std::hash<uint64_t>{}(combined);
    }
};

// 等価比較演算子の定義
struct PairEqual {
    template <class T1, class T2>
    bool operator()(const std::pair<T1, T2>& lhs, const std::pair<T1, T2>& rhs) const {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }
};

// 対称変換を文字列ではなく小さなIDで保持し、探索ホットパスでの
// 文字列生成・ハッシュ検索・std::function呼び出しを避ける。
enum class TransformId : uint8_t {
    identity = 0,
    rotate_90,
    rotate_180,
    rotate_270,
    flip_vertical,
    flip_horizontal,
    flip_diag_a1h8,
    flip_diag_a8h1,
    child_not_found = 0xFF
};

struct BookPosition;

struct Position {
    uint64_t my_stones = 0;
    uint64_t opponent_stones = 0;
    boost::container::small_vector<Link, 1> links;
    Leaf leaf = { 65, -64, false };
    int8_t eval_value = 0;
    // book_positionsは探索開始前に全件読み込み、探索中は追加しない。
    // そのため、この参照先と正規化変換を子Positionへ引き継げる。
    BookPosition* book_record = nullptr;
    TransformId book_transform = TransformId::identity;
};

// The board is already the hash-map key. Keeping it again in every stored
// value wastes 16 bytes per book entry, so only traversal positions carry it.
struct BookPosition {
    // Link records live in one contiguous arena; offsets remain valid if the arena
    // reallocates while the file is loading. The arena is immutable after loading.
    uint64_t link_offset = 0;
    uint8_t link_count = 0;
    Leaf leaf = { 65, -64, false };
    int8_t eval_value = 0;
    // Automatic-depth DFS memo; 0xFF = unknown, 0xFE = currently visiting.
    // This byte fits the structure's existing tail padding on the current MSVC ABI.
    uint8_t auto_depth_cache = 0xFF;
};

std::vector<Link> book_link_data;

inline std::span<Link> book_links(BookPosition& position) noexcept {
    if (position.link_count == 0) return {};
    return { book_link_data.data() + position.link_offset, position.link_count };
}

inline std::span<const Link> book_links(const BookPosition& position) noexcept {
    if (position.link_count == 0) return {};
    return { book_link_data.data() + position.link_offset, position.link_count };
}

inline void append_book_links(BookPosition& position, std::span<const Link> links) {
    position.link_offset = book_link_data.size();
    position.link_count = static_cast<uint8_t>(links.size());
    book_link_data.insert(book_link_data.end(), links.begin(), links.end());
}

// Edax uses move 65 (NOMOVE) for an absent leaf. Move 0 is a1 and can be a
// legitimate zero-evaluation leaf, so it must not be treated as a sentinel.
inline bool has_real_leaf(const Leaf& leaf) noexcept {
    return leaf.move != 65;
}

// Book records are loaded before traversal starts. unordered_flat_map improves lookup
// locality, but rehashing invalidates pointers cached by Position, so do not insert,
// erase, or reserve entries after loading has completed.
using PositionMap = boost::unordered_flat_map<std::pair<uint64_t, uint64_t>, BookPosition, PairHash, PairEqual>;

// グローバル変数の宣言と定義
extern PositionMap book_positions;
PositionMap book_positions;

class PositionManager {
public:
    // ログレベル一覧
    enum class LogLevel {
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        NONE
    };

    // 時間カウントとループ回数測定
    std::chrono::steady_clock::time_point program_start_time;
    size_t loop_count = 0;
    size_t next_progress_update = 1;

    // ポジションマネージャーの変数宣言部分
    std::string book_path;
    std::string debug_log_path;
    mutable LogLevel log_level;
    bool auto_adjust_log_level;
    LogLevel adjusted_log_level;

    // config.ini で指定するコンソール進捗表示の更新間隔。
    // 例えば 100000 なら、100000ループごとに表示を更新する。
    size_t progress_update_interval = 100000;

    // 初期設定等
    PositionManager(const std::string& book_path, const std::string& debug_log_path,
        LogLevel level = LogLevel::ERROR,
        bool auto_adjust = false,
        LogLevel adjusted_level = LogLevel::INFO,
        size_t progress_interval = 100000)
        : book_path(book_path),
        debug_log_path(debug_log_path),
        log_level(level),
        auto_adjust_log_level(auto_adjust),
        adjusted_log_level(adjusted_level),
        progress_update_interval(progress_interval) {
        init_debug_log();
    }

    // デバッグログ出力関数本体
    void debug_log(const std::string& message, LogLevel level, bool is_adjustment_message = false) {
        if (level >= log_level) {
            std::ofstream log_file(debug_log_path, std::ios_base::app | std::ios_base::binary);
            if (log_file.is_open()) {
                log_file << message << std::endl;

                // WARNING 以上のレベルでログ出力された場合、ログレベルを自動調整
                if (!is_adjustment_message && auto_adjust_log_level && level >= LogLevel::WARNING && log_level > adjusted_log_level) {
                    LogLevel previous_level = log_level;
                    log_level = adjusted_log_level;

                    std::string warning_message = "Log level automatically adjusted from "
                        + log_level_to_string(previous_level) + " to "
                        + log_level_to_string(log_level);

                    // 調整メッセージを直接書き込み、再帰呼び出しを避ける
                    log_file << warning_message << std::endl;
                }
            }
        }
    }

    bool is_log_enabled(LogLevel level) const noexcept {
        return level >= log_level;
    }

    // デバッグログ出力用
private:
    void init_debug_log() {
        std::ofstream log_file(debug_log_path, std::ios_base::trunc | std::ios_base::binary);
        if (log_file.is_open()) {
            // UTF-8 BOMを書き込む
            log_file << static_cast<char>(0xEF) << static_cast<char>(0xBB) << static_cast<char>(0xBF);
            //時刻を記録
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            std::tm local_tm;
            localtime_s(&local_tm, &now_c);

            log_file << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << "] "
                << "[" << log_level_to_string(log_level) << "]" << std::endl;
        }
    }

private:
    // const 修飾子を追加　デバッグ出力用
    std::string log_level_to_string(LogLevel level) const {
        switch (level) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::INFO: return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
        default: return "UNKNOWN";
        }
    }
};

// Open the result file only after the first result and keep it buffered for the
// lifetime of one search. Opening and flushing once per finding dominates runs
// that produce many rows.
class ResultWriter {
public:
    ResultWriter(const std::string& path, PositionManager& manager)
        : path_(path), manager_(manager) {}

    void append_line(const std::string& line) {
        if (!ensure_open()) return;
        output_ << line << '\n';
        if (!output_) {
            manager_.debug_log("Failed to write output file: " + path_, PositionManager::LogLevel::ERROR);
            failed_ = true;
        }
    }

    // Write a candidate move without first allocating/copying a full kifu string.
    // Pass (64) has no coordinate suffix in the output format.
    void append_kifu_candidate(const std::string& prefix, uint8_t move) {
        if (!ensure_open()) return;
        output_ << prefix;
        if (move != 64) {
            const char coordinate[2] = {
                static_cast<char>('a' + (move % 8)),
                static_cast<char>('0' + (move / 8) + 1)
            };
            output_.write(coordinate, sizeof(coordinate));
        }
        output_.put('\n');
        if (!output_) {
            manager_.debug_log("Failed to write output file: " + path_, PositionManager::LogLevel::ERROR);
            failed_ = true;
        }
    }

private:
    bool ensure_open() {
        if (failed_) return false;
        if (opened_) return true;
        output_.open(path_, std::ios::app | std::ios::binary);
        if (!output_.is_open()) {
            manager_.debug_log("Failed to open or create output file: " + path_, PositionManager::LogLevel::ERROR);
            failed_ = true;
            return false;
        }
        output_.seekp(0, std::ios::end);
        if (output_.tellp() == 0) {
            output_ << static_cast<char>(0xEF) << static_cast<char>(0xBB) << static_cast<char>(0xBF);
        }
        opened_ = true;
        return true;
    }

    std::string path_;
    PositionManager& manager_;
    std::ofstream output_;
    bool opened_ = false;
    bool failed_ = false;
};

// config.ini 読み込み関数
// Config から、ログ設定・mode・1手あたりの誤差・積算誤差・初期局面から探索する最大Book plyを読み込む。
// per_move_error / cumulative_error / max_book_move_depth が -1 の場合は、それぞれの制限を無効にする。
// progress_update_interval は1以上の整数で、コンソール進捗表示の更新間隔を指定する。
std::tuple<PositionManager::LogLevel, bool, PositionManager::LogLevel, int, int, int, int, bool, int> read_config(const std::string& config_path) {
    // 設定ファイルを開く
    std::ifstream config_file(config_path);
    std::string line;

    // デフォルト値の設定
    PositionManager::LogLevel log_level = PositionManager::LogLevel::ERROR;
    bool auto_adjust = false;
    PositionManager::LogLevel adjusted_level = PositionManager::LogLevel::INFO;
    int mode = 4;  // デフォルトモードを4に設定
    int per_move_error = -1;     // -1: 制限なし。0以上: 1手で許容する最大の石損数。
    int cumulative_error = -1;   // -1: 制限なし。0以上: 黒+白の積算石損数に対する最大値。
    int max_book_move_depth = -1; // -1: 制限なし。0以上: 初期局面から探索する最大 ply。
    bool auto_detect_book_depth = false; // true: book内の最深到達positionから上限を自動算出。
    int progress_update_interval = 100000; // コンソール進捗表示を更新するループ間隔。

    // ログレベルの文字列と列挙型のマッピング
    std::unordered_map<std::string, PositionManager::LogLevel> log_level_map = {
        {"DEBUG", PositionManager::LogLevel::DEBUG},
        {"INFO", PositionManager::LogLevel::INFO},
        {"WARNING", PositionManager::LogLevel::WARNING},
        {"ERROR", PositionManager::LogLevel::ERROR},
        {"NONE", PositionManager::LogLevel::NONE}
    };

    // 設定ファイルを1行ずつ読み込む
    while (std::getline(config_file, line)) {
        // ログレベルの設定を読み込む
        if (line.substr(0, 9) == "log_level") {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string level = line.substr(pos + 1);
                level.erase(0, level.find_first_not_of(" \t"));
                level.erase(level.find_last_not_of(" \t") + 1);
                auto it = log_level_map.find(level);
                if (it != log_level_map.end()) {
                    log_level = it->second;
                }
            }
        }
        // 自動調整レベルの設定を読み込む
        else if (line.substr(0, 18) == "auto_adjust_level=") {
            std::string value = line.substr(18);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return std::tolower(c); });
            auto_adjust = (value == "true");
        }
        // 調整後のログレベルの設定を読み込む
        else if (line.substr(0, 15) == "adjusted_level=") {
            std::string level = line.substr(15);
            level.erase(0, level.find_first_not_of(" \t"));
            level.erase(level.find_last_not_of(" \t") + 1);
            auto it = log_level_map.find(level);
            if (it != log_level_map.end()) {
                adjusted_level = it->second;
            }
        }
        // モードの設定を読み込む
        else if (line.substr(0, 5) == "mode=") {
            mode = std::stoi(line.substr(5));
        }
        // 1手あたりの誤差設定を読み込む。
        // 例: per_move_error = 2 なら、1手で2石損までを許容する。
        else if (line.rfind("per_move_error", 0) == 0) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                per_move_error = std::stoi(line.substr(pos + 1));
            }
        }
        // 積算誤差設定を読み込む。
        // 黒・白それぞれの積算石損数を管理し、その合計を上限として使用する。
        else if (line.rfind("cumulative_error", 0) == 0) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                cumulative_error = std::stoi(line.substr(pos + 1));
            }
        }
        // 初期局面から探索する最大 ply を読み込む。Pass も1 plyとして数える。
        // -1 は無制限。auto_detect_book_depth=true の場合はこの値を使わない。
        else if (line.rfind("max_book_move_depth", 0) == 0) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                max_book_move_depth = std::stoi(line.substr(pos + 1));
            }
        }
        // Bookグラフから探索上限を自動判定する設定を読み込む。
        else if (line.rfind("auto_detect_book_depth", 0) == 0) {
            const size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                std::transform(value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (value == "true") auto_detect_book_depth = true;
                else if (value == "false") auto_detect_book_depth = false;
                else throw std::invalid_argument("auto_detect_book_depth must be True or False.");
            }
        }
        // コンソール進捗表示の更新間隔を読み込む。
        // 例: progress_update_interval = 10000 なら10000ループごとに表示を更新する。
        else if (line.rfind("progress_update_interval", 0) == 0) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                progress_update_interval = std::stoi(line.substr(pos + 1));
            }
        }
    }

    // 返値: ログ設定、mode、誤差制限、手数上限、自動深さフラグ、進捗表示間隔。
    return std::make_tuple(log_level, auto_adjust, adjusted_level, mode, per_move_error,
        cumulative_error, max_book_move_depth, auto_detect_book_depth, progress_update_interval);
}

// move値の実際の実装が説明と異なる部分があるための修正用
inline uint8_t rotate_move_180(uint8_t move) {
    if (move >= 64) {
        return move;
    }
    return 63 - move;
}

// BookPositionに必要な項目だけを抽出し、全レコードをPositionMapへ読み込む。
void load_all_positions(const std::string& book_path, PositionManager& manager) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // ファイルマッピングを作成
    boost::interprocess::file_mapping file(book_path.c_str(), boost::interprocess::read_only);
    boost::interprocess::mapped_region region(file, boost::interprocess::read_only);

    // マップされたリージョンの先頭ポインタを取得
    const char* data = static_cast<const char*>(region.get_address());
    std::size_t filesize = region.get_size();
    manager.debug_log("File size: " + std::to_string(filesize) + " bytes", PositionManager::LogLevel::INFO);

    // ポジション数を推定
    constexpr double avg_position_size = 44.0720;
    double estimated_positions_double = static_cast<double>(filesize) / avg_position_size;
    size_t estimated_positions = static_cast<size_t>(estimated_positions_double);

    // Reserve elements (not buckets); flat_map chooses its own slot capacity.
    size_t estimated_reserve_positions = static_cast<size_t>(estimated_positions * 1.10);
    // Each book record has 42 fixed bytes plus two bytes per link. Estimating
    // from the same measured average record size avoids repeated arena growth.
    const double estimated_links_double = std::max(0.0,
        (static_cast<double>(filesize) - 42.0 - estimated_positions * 42.0) / 2.0);
    // Keep a modest safety margin: vector growth near 700 million links would
    // temporarily retain two very large allocations while reallocation occurs.
    const size_t estimated_links = static_cast<size_t>(estimated_links_double * 1.05);

    manager.debug_log("Estimated number of elements to reserve: " + std::to_string(estimated_reserve_positions), PositionManager::LogLevel::DEBUG);
    book_positions.reserve(estimated_reserve_positions);
    book_link_data.clear();
    book_link_data.reserve(estimated_links);

    if (manager.log_level == PositionManager::LogLevel::DEBUG) {
        const size_t flat_capacity = book_positions.bucket_count();
        const size_t group_count = (flat_capacity + 1) / 15;
        const size_t slot_storage_bytes = flat_capacity * sizeof(PositionMap::value_type);
        // Boost 1.85 flat_map stores 15 slots per group with 16 control bytes.
        const size_t control_storage_bytes = group_count * 16;
        manager.debug_log("Actual flat-map capacity after reserve: " + std::to_string(flat_capacity), PositionManager::LogLevel::DEBUG);
        manager.debug_log("Estimated number of positions: " + std::to_string(estimated_positions), PositionManager::LogLevel::DEBUG);
        const double reserved_table_memory_mb = (slot_storage_bytes + control_storage_bytes) / (1024.0 * 1024.0);
        manager.debug_log("Estimated reserved flat-map table storage: " + std::to_string(reserved_table_memory_mb) +
            " MB (value slots plus control bytes; excludes separately allocated links and allocator overhead)",
            PositionManager::LogLevel::DEBUG);
    }

    // ヘッダーをスキップ
    const char* current = data + 42;

    // 読み込み時間の測定
    auto read_start_time = std::chrono::high_resolution_clock::now();

    // 変数の初期化
    size_t positions_loaded = 0;

    while (current < data + filesize) {
        uint64_t my_stones = *reinterpret_cast<const uint64_t*>(current);
        current += sizeof(uint64_t);
        uint64_t opponent_stones = *reinterpret_cast<const uint64_t*>(current);
        current += sizeof(uint64_t);

        current += 16;  // win, draw, lose, lineをスキップ

        int16_t raw_value = *reinterpret_cast<const int16_t*>(current);
        current += sizeof(int16_t);

        current += 4;  // minvalue, maxvalueをスキップ

        uint8_t numberline = *reinterpret_cast<const uint8_t*>(current);
        current += sizeof(uint8_t);

        current += 1;  // levelをスキップ

        // Insert before parsing the variable-size link list so duplicate records
        // do not leave unreachable links in the contiguous arena.
        auto [position_it, inserted] = book_positions.try_emplace(
            std::make_pair(my_stones, opponent_stones));
        BookPosition& position = position_it->second;
        if (inserted) {
            position.link_offset = book_link_data.size();
            position.link_count = numberline;
        }

        // 評価値が範囲外だった場合
        if (raw_value < -127 || raw_value > 127) {
            manager.debug_log("Error: Value out of int8_t range: " + std::to_string(raw_value), PositionManager::LogLevel::ERROR);
            std::exit(1);
        }
        int8_t value = static_cast<int8_t>(raw_value);

        // リンクとリーフの処理
        for (int i = 0; i < numberline; ++i) {
            int8_t link_value = *reinterpret_cast<const int8_t*>(current);
            current += sizeof(int8_t);
            uint8_t link_move = *reinterpret_cast<const uint8_t*>(current);
            current += sizeof(uint8_t);
            if (inserted) {
                book_link_data.push_back({ rotate_move_180(link_move), link_value, false });
            }
        }

        int8_t leaf_eval = *reinterpret_cast<const int8_t*>(current);
        current += sizeof(int8_t);
        uint8_t leaf_move = *reinterpret_cast<const uint8_t*>(current);
        current += sizeof(uint8_t);

        if (inserted) {
            position.leaf = { rotate_move_180(leaf_move), leaf_eval, false };
            position.eval_value = value;
        }
        positions_loaded++;

        // book の読み込み進捗は従来どおり10万positionごとに表示する。
        // progress_update_interval は探索ループの表示更新だけを制御し、初期読み込みの動作は変更しない。
        if (positions_loaded % 100000 == 0) {
            std::cout << "\r" << positions_loaded << " Loading Completed" << std::flush;
        }
    }

    // 最終的な読み込み数を表示
    std::cout << "\r" << positions_loaded << " Loading Completed" << std::endl;

    // 読み込み時間測定終了
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);

    manager.debug_log("Actual number of positions loaded: " + std::to_string(positions_loaded), PositionManager::LogLevel::INFO);

    if (manager.log_level == PositionManager::LogLevel::DEBUG) {
        // The flat table stores compact offsets/counts; links share one arena.
        const size_t flat_capacity = book_positions.bucket_count();
        const size_t group_count = (flat_capacity + 1) / 15;
        const size_t slot_storage_bytes = flat_capacity * sizeof(PositionMap::value_type);
        const size_t control_storage_bytes = group_count * 16;
        const size_t links_arena_bytes = book_link_data.capacity() * sizeof(Link);

        const size_t estimated_total_memory = slot_storage_bytes + control_storage_bytes + links_arena_bytes;

        std::stringstream ss;
        ss << "Estimated memory usage of book_positions (allocator overhead excluded):"
            << "\n  Flat-map slot storage: " << slot_storage_bytes << " bytes"
            << "\n  Flat-map control storage: " << control_storage_bytes << " bytes"
            << "\n  Contiguous link arena capacity: " << links_arena_bytes << " bytes"
            << "\n  Estimated total: " << estimated_total_memory << " bytes"
            << "\n  Estimated total (MB): " << (estimated_total_memory / (1024.0 * 1024.0)) << " MB";
        manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);

        // Position構造体のサイズを出力
        ss.str("");
        ss << "Size of structures:"
            << "\n  Traversal Position: " << sizeof(Position) << " bytes"
            << "\n  Stored BookPosition: " << sizeof(BookPosition) << " bytes"
            << "\n  Link: " << sizeof(Link) << " bytes"
            << "\n  Leaf: " << sizeof(Leaf) << " bytes"
            << "\n  BookPosition link offset/count: " << sizeof(BookPosition::link_offset) + sizeof(BookPosition::link_count) << " bytes";
        manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);

        manager.debug_log("Boost unordered_flat_map does not expose per-group probe collision counts through its public API; use the offline hash-distribution benchmark for collision analysis.", PositionManager::LogLevel::DEBUG);
    }

    // ファイル読み込み時間の測定
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    manager.debug_log("File I/O time: " + std::to_string(read_duration.count()) + " ms", PositionManager::LogLevel::INFO);
    manager.debug_log("Total load time: " + std::to_string(total_duration.count()) + " ms", PositionManager::LogLevel::INFO);
}

// デバッグログの整地
std::string format_position(const Position& position) {
    std::stringstream ss;
    ss << "my_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << position.my_stones
        << ", opponent_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << position.opponent_stones
        << ", eval_value: " << std::dec << static_cast<int>(position.eval_value)
        << "\nLinks: ";
    for (const auto& link : position.links) {
        ss << "{move: " << static_cast<int>(link.move)
            << ", eval_link: " << static_cast<int>(link.eval_link)
            << ", visited: " << (link.visited ? "True" : "False") << "} ";
    }
    ss << "\nLeaf: {move: " << static_cast<int>(position.leaf.move)
        << ", eval: " << static_cast<int>(position.leaf.eval)
        << ", visited: " << (position.leaf.visited ? "True" : "False") << "}";

    // 返値: 盤面の状態を表す文字列
    return ss.str();
}

std::string format_book_position(uint64_t my_stones, uint64_t opponent_stones, const BookPosition& position) {
    std::stringstream ss;
    ss << "my_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << my_stones
        << ", opponent_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << opponent_stones
        << ", eval_value: " << std::dec << static_cast<int>(position.eval_value)
        << "\nLinks: ";
    for (const auto& link : book_links(position)) {
        ss << "{move: " << static_cast<int>(link.move)
            << ", eval_link: " << static_cast<int>(link.eval_link)
            << ", visited: " << (link.visited ? "True" : "False") << "} ";
    }
    ss << "\nLeaf: {move: " << static_cast<int>(position.leaf.move)
        << ", eval: " << static_cast<int>(position.leaf.eval)
        << ", visited: " << (position.leaf.visited ? "True" : "False") << "}";
    return ss.str();
}

// 各関数の宣言
std::tuple<Position, TransformId, uint8_t> get_children(PositionManager& manager, Position& position, std::string& kifu);
std::tuple<Position, TransformId> process_position(Position& position, std::string& kifu, uint8_t move, PositionManager& manager);
Position create_position_data(const Position& position, std::string& kifu, PositionManager& manager, int move = -1);
void append_move_to_kifu(std::string& kifu, int move, PositionManager& manager);
Position flip_stones(const Position& position, int move);
uint64_t shift(uint64_t b, int dir);
// Mode 6/7 用：盤面からオセロの合法手（配置手）のビットマスクを生成する。
uint64_t generate_legal_moves(const Position& position);
std::tuple<std::pair<uint64_t, uint64_t>, TransformId> normalize_position(uint64_t my_stones, uint64_t opponent_stones, PositionManager& manager);
int denormalize_move(int move, TransformId transformation, PositionManager& manager);
int rotate_move_90(int move);
int rotate_move_270(int move);
int flip_move_vertical(int move);
int flip_move_horizontal(int move);
int flip_move_diag_a1h8(int move);
int flip_move_diag_a8h1(int move);
int normalize_move(int move, TransformId transformation, PositionManager& manager);
BookPosition* read_position(uint64_t my_stones, uint64_t opponent_stones);
// ================================================================
// 1手あたりの誤差・積算誤差を探索中に管理する状態。
// black_loss / white_loss は初期局面から見た黒・白それぞれの累積石損数。
// black_to_move は「現在の Position で手を指す側」が初期局面の黒かどうかを表す。
// ================================================================
struct ErrorState {
    int black_loss = 0;
    int white_loss = 0;
    bool black_to_move = true;
};

// 現在のPositionから次のleafを探索してよいか判定する。
// move_number は初期局面から現在のPositionまでに実際に指された手数（ply）で、
// Passも1手として数える。棋譜文字列は既存仕様によりPassを除去しているため、
// 棋譜文字数から手数を計算せず、明示的なmove_numberを使用する。
// max_book_move_depth=-1 は無制限。
inline bool can_search_next_book_move(int move_number, int max_book_move_depth) {
    if (max_book_move_depth < 0) {
        return true;
    }
    return move_number < max_book_move_depth;
}

// 探索ループの共通進捗表示。
// mode 1～4 と mode 6～7 で同じ処理を使い、更新間隔だけ config.ini から変更できるようにする。
inline void update_search_progress(PositionManager& manager) {
    ++manager.loop_count;

    const size_t interval = manager.progress_update_interval;
    if (manager.loop_count == 1 ||
        (interval > 0 && manager.loop_count >= manager.next_progress_update)) {
        std::cout << "\r" << manager.loop_count << " Links or Leaf processed" << std::flush;
        manager.next_progress_update = (manager.loop_count == 1)
            ? interval
            : manager.loop_count + interval;
    }
}

void mismatch_process(const Position& child_position, const std::string& kifu, ResultWriter& output, PositionManager& manager, int8_t child_eval, int8_t parent_eval, int mode, int per_move_error, int cumulative_error, const ErrorState& child_error_state);
int8_t calculate_parent_eval(const Position& parent_position, uint8_t move, PositionManager& manager);

// 親 Position から指定の手へ進んだ場合の石損数を計算し、1手あたりの誤差・積算誤差の制限を判定する。
// move_eval は親側から見たその手の評価値（link.eval_link / leaf.eval）。
// Pass は強制手であり評価損として数えず、手番だけ反転する。
bool try_apply_error_limits(const Position& parent_position, uint8_t move, int8_t move_eval, const ErrorState& current_state, int per_move_error, int cumulative_error, ErrorState& next_state, int& move_loss);

// 探索設定をまとめた構造体。const 参照で再帰関数へ渡すので、ノードごとの引数管理を簡潔にする。
struct SearchSettings {
    int mode;
    int per_move_error;
    int cumulative_error;
    int max_book_move_depth;

    inline bool error_limits_enabled() const {
        return per_move_error >= 0 || cumulative_error >= 0;
    }

    inline bool missing_position_mode() const {
        return mode == 6 || mode == 7;
    }
};

void main_process_recursive(Position& current_position, std::string& current_kifu, ResultWriter& output, PositionManager& manager, const SearchSettings& settings, std::set<std::string>& reported_kifus, int move_number, const ErrorState& error_state);

// Mode 6/7 の追加処理。既存の book 読み込み・子ポジション生成・棋譜生成を流用し、
// 「leaf の次の position が欠けているが、その次の一手を指した position は book に存在する」
// パターンを検出するために使用する。
bool has_unvisited_book_move(const Position& position);
// Mode 6/7 で必要になる座標付き棋譜を、manager を変更せずに生成する。Pass は既存仕様に合わせて文字列へ追加しない。
std::string append_move_to_kifu(const std::string& kifu, int move);
// Mode 6/7 共通の結果出力関数。
// mode=6 は合法手後、mode=7 は合法手直前の棋譜を出力する。
void output_mode6_7_kifu(const std::string& kifu, ResultWriter& output, PositionManager& manager, std::set<std::string>& reported_kifus, int mode);
// Mode 6/7 共通の欠落 leaf 検査。
void scan_missing_leaf_mode6_7(const Position& leaf_parent, const std::string& kifu, ResultWriter& output, PositionManager& manager, std::set<std::string>& reported_kifus, const SearchSettings& settings);

// 親ポジションのリンクやリーフのうち最良のものの評価値を取得する関数
inline int8_t calculate_parent_eval(const Position& parent_position, uint8_t move, PositionManager& manager) {
    int8_t parent_eval = -64;// -64で初期化

    // 親ポジションの該当するリンクの評価値を検索
    for (const auto& link : parent_position.links) {
        if (link.move == move) {
            parent_eval = link.eval_link;
            return parent_eval;
        }
    }

    // リーフの評価値から取得した場合、INFOレベルのデバッグログを出力
    if (parent_position.leaf.move == move) {
        parent_eval = parent_position.leaf.eval;
        if (manager.is_log_enabled(PositionManager::LogLevel::INFO)) {
            manager.debug_log("Found matching leaf - move: " + std::to_string(static_cast<int>(move)) +
                ", parent_eval: " + std::to_string(static_cast<int>(parent_eval)), PositionManager::LogLevel::INFO);
        }
    }

    // 返値 親の評価値
    return parent_eval;
}

// ================================================================
// 1手あたりの誤差・積算誤差の共通判定。
// 石損 = 親 Position の評価値 - その手の評価値。負になった場合は0とする。
// 例：親評価値0、着手評価値-2なら2石損なので、per_move_error=2では許容される。
// 積算誤差は、黒・白それぞれの累積石損数を保持し、その合計を共通の上限として判定する。
// そのため cumulative_error=6 なら、黒4+白0の時点では残り2をどちらでも使えるが、
// その2を使った後は両者とも石損0（最善手のみ）となる。
// ================================================================
bool try_apply_error_limits(const Position& parent_position, uint8_t move, int8_t move_eval, const ErrorState& current_state, int per_move_error, int cumulative_error, ErrorState& next_state, int& move_loss) {
    // 両方とも無制限なら評価値計算・枝切りを行わず、手番だけを進める。
    // これにより、従来どおりの無制限探索時のオーバーヘッドを最小限にする。
    if (per_move_error < 0 && cumulative_error < 0) {
        move_loss = 0;
        next_state = current_state;
        next_state.black_to_move = !current_state.black_to_move;
        return true;
    }

    // Pass は強制手なので、選択による石損は0として扱う。
    if (move == 64) {
        move_loss = 0;
    }
    else {
        move_loss = static_cast<int>(parent_position.eval_value) - static_cast<int>(move_eval);
        if (move_loss < 0) {
            // Book の不整合等で見かけ上「得をする」手になっている場合、損失は0とする。
            move_loss = 0;
        }
    }

    // 1手あたりの誤差が設定されている場合は、1手の石損数を超える枝を切る。
    if (per_move_error >= 0 && move_loss > per_move_error) {
        next_state = current_state;
        next_state.black_to_move = !current_state.black_to_move;
        return false;
    }

    // 次の黒・白それぞれの累積石損数を更新する。
    next_state = current_state;
    if (current_state.black_to_move) {
        next_state.black_loss += move_loss;
    }
    else {
        next_state.white_loss += move_loss;
    }

    // 積算誤差は黒+白の合計を共通予算として判定する。
    const int next_total_loss = next_state.black_loss + next_state.white_loss;
    if (cumulative_error >= 0 && next_total_loss > cumulative_error) {
        return false;
    }

    // 次の Position では手番が反転する。
    next_state.black_to_move = !current_state.black_to_move;
    return true;
}

// 誤差制限で枝切りしたことを DEBUG ログへ残す。
void log_error_pruned_move(PositionManager& manager, uint8_t move, int move_loss, const ErrorState& state, int per_move_error, int cumulative_error) {
    if (!manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) return;
    std::stringstream ss;
    ss << "Error-limit prune: move=" << static_cast<int>(move)
       << ", move_loss=" << move_loss
       << ", black_loss=" << state.black_loss
       << ", white_loss=" << state.white_loss
       << ", total_loss=" << (state.black_loss + state.white_loss)
       << ", per_move_error=" << per_move_error
       << ", cumulative_error=" << cumulative_error;
    manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);
}

// ミスマッチ判定のための関数
bool judge_mismatch(const Position& child_position, const Position& parent_position, uint8_t move, int mode, PositionManager& manager) {
    const int8_t child_eval = child_position.eval_value;
    bool mismatch = false;
    const bool build_details = manager.is_log_enabled(PositionManager::LogLevel::DEBUG);
    std::string comparison_details;

    if (mode == 3) {
        const int8_t parent_eval = calculate_parent_eval(parent_position, move, manager);
        mismatch = parent_eval != -child_eval;
        if (build_details) {
            comparison_details = "Mode 3: parent_eval (" + std::to_string(parent_eval) +
                ") vs -child_eval (" + std::to_string(-child_eval) + ")";
        }
    }
    else {
        int8_t max_child_link_eval = INT8_MIN;
        for (const auto& link : child_position.links) {
            if (link.eval_link > max_child_link_eval) max_child_link_eval = link.eval_link;
        }

        switch (mode) {
        case 1:
            // A Mode 1 mismatch requires both a registered leaf and at least one link.
            if (!child_position.links.empty() && has_real_leaf(child_position.leaf)) {
                mismatch = child_position.leaf.eval > max_child_link_eval;
                if (build_details) {
                    comparison_details = "Mode 1: leaf_eval (" + std::to_string(child_position.leaf.eval) +
                        ") vs max_child_link_eval (" + std::to_string(max_child_link_eval) + ")";
                }
            }
            else if (build_details) {
                comparison_details = child_position.links.empty()
                    ? "Mode 1: No links present, skipping mismatch check"
                    : "Mode 1: No valid leaf present, skipping mismatch check";
            }
            break;
        case 2:
        case 4: {
            int8_t max_child_move_eval = INT8_MIN;
            bool has_child_move = false;
            for (const auto& link : child_position.links) {
                has_child_move = true;
                if (link.eval_link > max_child_move_eval) max_child_move_eval = link.eval_link;
            }
            if (has_real_leaf(child_position.leaf)) {
                has_child_move = true;
                if (child_position.leaf.eval > max_child_move_eval) max_child_move_eval = child_position.leaf.eval;
            }
            if (!has_child_move) {
                if (build_details) comparison_details = "Mode " + std::to_string(mode) + ": No valid child moves";
                break;
            }

            if (mode == 2) {
                mismatch = child_eval != max_child_move_eval;
                if (build_details) {
                    comparison_details = "Mode 2: child_eval (" + std::to_string(child_eval) +
                        ") vs max_child_move_eval (" + std::to_string(max_child_move_eval) + ")";
                }
            }
            else {
                const int8_t parent_eval = calculate_parent_eval(parent_position, move, manager);
                mismatch = parent_eval != -max_child_move_eval;
                if (build_details) {
                    comparison_details = "Mode 4: parent_eval (" + std::to_string(parent_eval) +
                        ") vs -max_child_move_eval (" + std::to_string(-max_child_move_eval) + ")";
                }
            }
            break;
        }
        }
    }

    if (build_details) {
        manager.debug_log(std::string(mismatch ? "Mismatch detected: " : "No mismatch: ") + comparison_details,
            PositionManager::LogLevel::DEBUG);
    }
    return mismatch;
}

// 不一致発見の場合の処理
void mismatch_process(const Position& child_position, const std::string& kifu,
    ResultWriter& output, PositionManager& manager, int8_t child_eval, int8_t parent_eval, int mode,
    int per_move_error, int cumulative_error, const ErrorState& child_error_state) {

    auto output_candidate_with_error_limit = [&](uint8_t candidate_move, int8_t candidate_eval, const std::string& log_prefix) {
        ErrorState next_candidate_state;
        int candidate_loss = 0;
        if (!try_apply_error_limits(child_position, candidate_move, candidate_eval, child_error_state,
            per_move_error, cumulative_error, next_candidate_state, candidate_loss)) {
            log_error_pruned_move(manager, candidate_move, candidate_loss, child_error_state, per_move_error, cumulative_error);
            return;
        }

        output.append_kifu_candidate(kifu, candidate_move);
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            std::string logged_kifu = kifu;
            if (candidate_move != 64) append_move_to_kifu(logged_kifu, candidate_move, manager);
            manager.debug_log(log_prefix + logged_kifu + " (Move: " + std::to_string(candidate_move) + ")",
                PositionManager::LogLevel::DEBUG);
        }
    };

    if (mode == 1) {
        // Mode 1 compares a real child leaf against the best link and reports the leaf's move.
        if (child_position.links.empty() || !has_real_leaf(child_position.leaf)) return;
        int8_t max_child_link_eval = INT8_MIN;
        for (const auto& link : child_position.links) {
            if (link.eval_link > max_child_link_eval) max_child_link_eval = link.eval_link;
        }
        if (child_position.leaf.eval > max_child_link_eval) {
            output_candidate_with_error_limit(child_position.leaf.move, child_position.leaf.eval,
                "Mismatch found (Mode 1, leaf move). Kifu: ");
        }
        return;
    }

    int8_t max_child_move_eval = INT8_MIN;
    bool has_child_move = false;
    for (const auto& link : child_position.links) {
        has_child_move = true;
        if (link.eval_link > max_child_move_eval) max_child_move_eval = link.eval_link;
    }
    if (has_real_leaf(child_position.leaf)) {
        has_child_move = true;
        if (child_position.leaf.eval > max_child_move_eval) max_child_move_eval = child_position.leaf.eval;
    }
    if (!has_child_move) return;

    bool is_greater = false;
    int8_t comparison_value = INT8_MIN;
    switch (mode) {
    case 2:
        is_greater = max_child_move_eval > child_eval;
        // Mode 2 compares the child's stored value directly with its best move.
        comparison_value = child_eval;
        break;
    case 3:
        is_greater = -child_eval > parent_eval;
        comparison_value = static_cast<int8_t>(-parent_eval);
        break;
    case 4:
        comparison_value = static_cast<int8_t>(-parent_eval);
        // If any child move crosses the negated parent-edge value, report all such moves.
        is_greater = max_child_move_eval > comparison_value;
        break;
    default:
        return;
    }

    if (is_greater) {
        for (const auto& link : child_position.links) {
            if (link.eval_link > comparison_value) {
                output_candidate_with_error_limit(link.move, link.eval_link,
                    "Mismatch found (multiple moves). Kifu: ");
            }
        }
        if (has_real_leaf(child_position.leaf) && child_position.leaf.eval > comparison_value) {
            output_candidate_with_error_limit(child_position.leaf.move, child_position.leaf.eval,
                "Mismatch found (leaf move). Kifu: ");
        }
    }
    else {
        uint8_t max_child_move = 65;
        for (const auto& link : child_position.links) {
            if (link.eval_link == max_child_move_eval) {
                max_child_move = link.move;
                break;
            }
        }
        if (has_real_leaf(child_position.leaf) && child_position.leaf.eval == max_child_move_eval) {
            max_child_move = child_position.leaf.move;
            manager.debug_log("Leaf evaluation used for max_child_move_eval", PositionManager::LogLevel::INFO);
        }
        if (max_child_move <= 64) {
            output_candidate_with_error_limit(max_child_move, max_child_move_eval,
                "Mismatch found (single move). Kifu: ");
        }
    }
}

std::tuple<Position, TransformId, uint8_t> get_children(PositionManager& manager, Position& position, std::string& kifu) {
    try {
        const std::size_t parent_kifu_size = kifu.size();

        // リンクの処理
        for (auto& link : position.links) {
            if (!link.visited) {
                link.visited = true;  // リンクを訪問済みにマーク
                if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
                    manager.debug_log("Unvisited link found: Move=" + std::to_string(link.move) + ", Eval=" + std::to_string(link.eval_link) + ", Visited: False", PositionManager::LogLevel::DEBUG);
                }
                auto [child_position, transformation] = process_position(position, kifu, link.move, manager);
                if (transformation != TransformId::child_not_found) {
                    // 成功時はkifuに今回の手が追加された状態で返す。
                    return std::make_tuple(std::move(child_position), std::move(transformation), link.move);
                }
                // bookにない子だった場合、この候補の棋譜だけ巻き戻して次の兄弟へ進む。
                kifu.resize(parent_kifu_size);
            }
        }

        // リーフの処理
        if (position.leaf.move != 65) {
            if (!position.leaf.visited) {
                position.leaf.visited = true;  // リーフを訪問済みにマーク
                if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
                    manager.debug_log("Unvisited leaf found: Move=" + std::to_string(position.leaf.move) + ", Eval=" + std::to_string(position.leaf.eval) + ", Visited: False", PositionManager::LogLevel::DEBUG);
                }
                auto [child_position, transformation] = process_position(position, kifu, position.leaf.move, manager);
                if (transformation != TransformId::child_not_found) {
                    // 成功時はkifuに今回の手が追加された状態で返す。
                    return std::make_tuple(std::move(child_position), std::move(transformation), position.leaf.move);
                }
                kifu.resize(parent_kifu_size);
            }
        }
        // move値が65 noneの場合は処理をスキップしてデバッグ出力のみ
        else if (position.leaf.move == 65) {
            manager.debug_log("Leaf with move value 65 encountered. Skipping processing.", PositionManager::LogLevel::DEBUG);
        }
        // 全候補がbookにない場合も、呼び出し元の棋譜長に戻して返す。
        kifu.resize(parent_kifu_size);
        return std::make_tuple(Position(), TransformId::child_not_found, static_cast<uint8_t>(0));
    }
    catch (const std::exception& e) {
        manager.debug_log("Critical error in get_children: " + std::string(e.what()), PositionManager::LogLevel::ERROR);
        std::cerr << "Critical error: " << e.what() << std::endl;
        std::exit(1);  // プログラムを終了
    }
}

std::tuple<Position, TransformId> process_position(Position& position, std::string& kifu, uint8_t move, PositionManager& manager) {

    // 子ポジションを生成し、一時変数に保存する
    Position original_child_position = create_position_data(position, kifu, manager, move);
    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        manager.debug_log("Generated original child position: " + format_position(original_child_position), PositionManager::LogLevel::DEBUG);
        manager.debug_log("New kifu: " + kifu, PositionManager::LogLevel::DEBUG);
    }

    // 同じ親Positionから兄弟を順に調べるため、正規化変換とbookレコードを
    // Positionに保持して再利用する。探索中はbook_positionsへ挿入しない。
    TransformId parent_transformation = position.book_transform;
    BookPosition* normalized_parent_position = position.book_record;
    if (!normalized_parent_position) {
        const auto [normalized_parent, transformation] =
            normalize_position(position.my_stones, position.opponent_stones, manager);
        parent_transformation = transformation;
        normalized_parent_position = read_position(normalized_parent.first, normalized_parent.second);
        if (!normalized_parent_position) {
            manager.debug_log("Critical error: Parent position not found in book", PositionManager::LogLevel::ERROR);
            std::cerr << "Critical error: Parent position not found in book. Terminating program." << std::endl;
            std::exit(1);
        }
        position.book_record = normalized_parent_position;
        position.book_transform = parent_transformation;
    }
    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        const auto normalized_parent = std::get<0>(
            normalize_position(position.my_stones, position.opponent_stones, manager));
        manager.debug_log("Book position retrieved: " + format_book_position(normalized_parent.first,
            normalized_parent.second, *normalized_parent_position), PositionManager::LogLevel::DEBUG);
    }

    // 正規化された親ポジションの該当する手のVisitedフラグを直接更新 直接更新したいのでconstが付いているread_position関数は使えない
    uint8_t normalized_move = normalize_move(move, parent_transformation, manager);
    BookPosition& book_position = *normalized_parent_position;
    bool updated = false;
    for (Link& link : book_links(book_position)) {
        if (link.move == normalized_move) {
            link.visited = true;
            if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
                manager.debug_log("Parent link visited flag updated: move=" + std::to_string(normalized_move) + ", visited=True", PositionManager::LogLevel::DEBUG);
            }
            updated = true;
            break;
        }
    }
    // リーフも同様に処理
    if (!updated && book_position.leaf.move == normalized_move) {
        book_position.leaf.visited = true;
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Parent leaf visited flag updated: move=" + std::to_string(normalized_move) + ", visited=True", PositionManager::LogLevel::DEBUG);
        }
        updated = true;
    }
    // 更新された正規化親ポジションをbook_positionに直接保存
    if (updated && manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        const auto normalized_parent = std::get<0>(
            normalize_position(position.my_stones, position.opponent_stones, manager));
        manager.debug_log("Updated parent book position: " + format_book_position(normalized_parent.first,
            normalized_parent.second, book_position), PositionManager::LogLevel::DEBUG);
    }

    // 子ポジションを正規化し、bookと照合する
    std::pair<uint64_t, uint64_t> normalized_child_position;
    TransformId transformation = TransformId::identity;
    std::tie(normalized_child_position, transformation) = normalize_position(original_child_position.my_stones, original_child_position.opponent_stones, manager);

    uint64_t normalized_child_my_stones = normalized_child_position.first;
    uint64_t normalized_child_opponent_stones = normalized_child_position.second;

    BookPosition* book_child_position = read_position(normalized_child_my_stones, normalized_child_opponent_stones);

    if (book_child_position) {
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Child position found in book: " + format_book_position(normalized_child_my_stones, normalized_child_opponent_stones, *book_child_position), PositionManager::LogLevel::DEBUG);
        }

        // bookから得られた情報を使って、正規化前の子ポジションを更新する
        const auto stored_links = book_links(*book_child_position);
        original_child_position.links.assign(stored_links.begin(), stored_links.end());
        original_child_position.leaf = book_child_position->leaf;
        original_child_position.eval_value = book_child_position->eval_value;
        original_child_position.book_record = book_child_position;
        original_child_position.book_transform = transformation;

        // move値を正規化前の状態に戻す
        for (auto& link : original_child_position.links) {
            link.move = denormalize_move(link.move, transformation, manager);
        }
        original_child_position.leaf.move = denormalize_move(original_child_position.leaf.move, transformation, manager);

        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Final denormalized child position: " + format_position(original_child_position), PositionManager::LogLevel::DEBUG);
        }

        // 返値: 正規化前の子ポジションと変換名。棋譜は共有バッファに保持する。
        return std::make_tuple(std::move(original_child_position), transformation);
    }
    else {
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            std::stringstream ss2;
            ss2 << "Child position not found in book: (my_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << normalized_child_my_stones
                << ", opponent_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << normalized_child_opponent_stones << ")";
            manager.debug_log(ss2.str(), PositionManager::LogLevel::DEBUG);
        }

        // 呼び出し元のget_children()が候補棋譜を巻き戻す。
        return std::make_tuple(Position(), TransformId::child_not_found);
    }
}
// moveの例外処理と関数二つの呼び出し
Position create_position_data(const Position& position, std::string& kifu, PositionManager& manager, int move) {

    if (move == 64) {  // パス
        kifu += "Pass";
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Pass move detected. New kifu: " + kifu, PositionManager::LogLevel::DEBUG);
        }

        // パスの場合はflip_stonesと同様の処理を行うが、石の反転は行わない
        Position child_position;
        child_position.my_stones = position.opponent_stones;
        child_position.opponent_stones = position.my_stones;
        child_position.eval_value = -position.eval_value;  // 評価値を反転

        return child_position;
    }
    // ここに65が来ることはあり得ないはず
    else if (move == 65) {  // 無効な手
        manager.debug_log("Invalid move (None) detected. Terminating program. New kifu: " + kifu + "None", PositionManager::LogLevel::ERROR);
        // プログラムを終了
        std::exit(1);
    }
    // 二つの関数を呼び出して子ポジションを実際に作るところ
    append_move_to_kifu(kifu, move, manager);

    Position child_position = flip_stones(position, move);
    child_position.links.clear();

    return child_position;
}
void append_move_to_kifu(std::string& kifu, int move, PositionManager& manager) {
    const char col = static_cast<char>('a' + (move % 8));
    const char row = static_cast<char>('0' + (move / 8) + 1);
    kifu.push_back(col);
    kifu.push_back(row);
    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        manager.debug_log("Updated kifu: " + kifu, PositionManager::LogLevel::DEBUG);
    }
}
// グローバル変数として定義
constexpr uint64_t direction_mask[8] = {
    0xfefefefefefefefe, 0x7f7f7f7f7f7f7f7f,
    0xffffffffffffffff, 0xffffffffffffffff,
    0x7f7f7f7f7f7f7f7f, 0xfefefefefefefefe,
    0xfefefefefefefefe, 0x7f7f7f7f7f7f7f7f
};
// ビットシフト
inline uint64_t shift(uint64_t b, int dir) {
    switch (dir) {
    case 0: return (b << 1) & direction_mask[0];
    case 1: return (b >> 1) & direction_mask[1];
    case 2: return b << 8;
    case 3: return b >> 8;
    case 4: return (b << 7) & direction_mask[4];
    case 5: return (b >> 7) & direction_mask[5];
    case 6: return (b << 9) & direction_mask[6];
    case 7: return (b >> 9) & direction_mask[7];
    default: return 0;
    }
}
//　1方向ひっくり返し
inline uint64_t flip_line(uint64_t player, uint64_t opponent, int dir, uint64_t move) {
    uint64_t mask = shift(move, dir) & opponent;
    mask |= shift(mask, dir) & opponent;
    mask |= shift(mask, dir) & opponent;
    mask |= shift(mask, dir) & opponent;
    mask |= shift(mask, dir) & opponent;
    mask |= shift(mask, dir) & opponent;
    uint64_t outflank = shift(mask, dir) & player;
    return outflank ? mask : 0;
}
//　全方向ひっくり返し
inline uint64_t flip_all_directions(uint64_t player, uint64_t opponent, uint64_t move) {
    return flip_line(player, opponent, 0, move) |
        flip_line(player, opponent, 1, move) |
        flip_line(player, opponent, 2, move) |
        flip_line(player, opponent, 3, move) |
        flip_line(player, opponent, 4, move) |
        flip_line(player, opponent, 5, move) |
        flip_line(player, opponent, 6, move) |
        flip_line(player, opponent, 7, move);
}

// ================================================================
// Mode 6 用の合法手生成
// 既存の shift() と bitboard 表現をそのまま利用して、現在手番の
// 「盤上に石を置けるマス」だけを求める。
// flip_all_directions() を64マスそれぞれに対して呼ぶより軽量な
// 方向別 mobility 計算を採用している。
// ================================================================
// ================================================================
// Mode 6/7 用の合法手生成
// ================================================================
// 生の bitboard を直接受け取る版。Mode 6/7 の probe では Position 一時オブジェクトを作らずに利用できる。
inline uint64_t generate_legal_moves(uint64_t player, uint64_t opponent) {
    const uint64_t empty = ~(player | opponent);
    uint64_t legal_moves = 0;

    // 各方向について「自分石 -> 相手石の連続列 -> 空きマス」の形を抽出する。
    for (int dir = 0; dir < 8; ++dir) {
        uint64_t x = shift(player, dir) & opponent;
        x |= shift(x, dir) & opponent;
        x |= shift(x, dir) & opponent;
        x |= shift(x, dir) & opponent;
        x |= shift(x, dir) & opponent;
        x |= shift(x, dir) & opponent;
        legal_moves |= shift(x, dir) & empty;
    }

    return legal_moves;
}

inline uint64_t generate_legal_moves(const Position& position) {
    return generate_legal_moves(position.my_stones, position.opponent_stones);
}

//　ひっくり返し関数本体
Position flip_stones(const Position& position, int move) {
    uint64_t my_stones = position.my_stones;
    uint64_t opponent_stones = position.opponent_stones;

    // Book move indices map to the reverse bitboard order used by this reader.
    const uint64_t move_bit = 1ULL << (63 - move);

    // 石のひっくり返し
    uint64_t flipped = flip_all_directions(my_stones, opponent_stones, move_bit);
    my_stones |= move_bit | flipped;
    opponent_stones ^= flipped;

    // 返値: 石を裏返した後の新しいポジション
    return Position{ opponent_stones, my_stones, {}, {0, 0, false}, static_cast<int8_t>(-position.eval_value) };
}

//　デルタ関数　これが早いらしい
template<uint64_t Mask, int Delta>
constexpr uint64_t delta_swap(uint64_t x) {
    uint64_t t = (x ^ (x >> Delta)) & Mask;
    return x ^ t ^ (t << Delta);
}
//　コンパイル時に処理してくれて早くなるらしい
constexpr uint64_t flip_horizontal(uint64_t x) {
    return delta_swap<0x5555555555555555ULL, 1>(
        delta_swap<0x3333333333333333ULL, 2>(
            delta_swap<0x0F0F0F0F0F0F0F0FULL, 4>(x)
        )
    );
}

constexpr uint64_t flip_vertical(uint64_t x) {
    return delta_swap<0x00000000FFFFFFFFULL, 32>(
        delta_swap<0x0000FFFF0000FFFFULL, 16>(
            delta_swap<0x00FF00FF00FF00FFULL, 8>(x)
        )
    );
}

constexpr uint64_t flip_diag_a1h8(uint64_t x) {
    return delta_swap<0x00000000F0F0F0F0ULL, 28>(
        delta_swap<0x0000CCCC0000CCCCULL, 14>(
            delta_swap<0x00AA00AA00AA00AAULL, 7>(x)
        )
    );
}

constexpr uint64_t flip_diag_a8h1(uint64_t x) {
    return delta_swap<0x000000000F0F0F0FULL, 36>(
        delta_swap<0x0000333300003333ULL, 18>(
            delta_swap<0x0055005500550055ULL, 9>(x)
        )
    );
}

constexpr uint64_t rotate_90(uint64_t x) {
    return flip_horizontal(flip_diag_a1h8(x));
}

constexpr uint64_t rotate_270(uint64_t x) {
    return flip_vertical(flip_diag_a1h8(x));
}

constexpr uint64_t rotate_180(uint64_t x) {
    return flip_vertical(flip_horizontal(x));
}

// 8つの盤面対称変換を固定配列として保持する。配列順は正規化候補の
// 優先順にもなり、同じ最小盤面になる対称局面では先に現れた変換を使う。
struct BoardTransform {
    TransformId id;
    uint64_t (*transform)(uint64_t);
};

inline uint64_t identity_board_transform(uint64_t x) {
    return x;
}

inline const std::array<BoardTransform, 8>& get_board_transforms() {
    static const std::array<BoardTransform, 8> transforms = {{
        {TransformId::identity, identity_board_transform},
        {TransformId::rotate_90, rotate_90},
        {TransformId::rotate_180, rotate_180},
        {TransformId::rotate_270, rotate_270},
        {TransformId::flip_vertical, flip_vertical},
        {TransformId::flip_horizontal, flip_horizontal},
        {TransformId::flip_diag_a1h8, flip_diag_a1h8},
        {TransformId::flip_diag_a8h1, flip_diag_a8h1}
    }};
    return transforms;
}

const char* transform_name(TransformId id) {
    switch (id) {
    case TransformId::identity: return "identity";
    case TransformId::rotate_90: return "rotate_90";
    case TransformId::rotate_180: return "rotate_180";
    case TransformId::rotate_270: return "rotate_270";
    case TransformId::flip_vertical: return "flip_vertical";
    case TransformId::flip_horizontal: return "flip_horizontal";
    case TransformId::flip_diag_a1h8: return "flip_diag_a1h8";
    case TransformId::flip_diag_a8h1: return "flip_diag_a8h1";
    default: return "child_not_found";
    }
}

// 局面を正規化し、適用した対称変換をIDで返す。
std::tuple<std::pair<uint64_t, uint64_t>, TransformId> normalize_position(
    uint64_t my_stones, uint64_t opponent_stones, PositionManager& manager) {
    std::pair<uint64_t, uint64_t> min_value{my_stones, opponent_stones};
    TransformId min_transformation = TransformId::identity;

    const auto& transforms = get_board_transforms();
    // identityはmin_valueの初期値として既に評価済みなので再計算しない。
    for (size_t i = 1; i < transforms.size(); ++i) {
        const auto& candidate = transforms[i];
        const std::pair<uint64_t, uint64_t> transformed{
            candidate.transform(my_stones), candidate.transform(opponent_stones)};
        if (transformed < min_value) {
            min_value = transformed;
            min_transformation = candidate.id;
        }
    }

    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        std::stringstream ss;
        ss << "Final min transformation: " << transform_name(min_transformation)
            << ", min_value: (my_stones=0x" << std::hex << std::setw(16) << std::setfill('0') << min_value.first
            << ", opponent_stones=0x" << std::setw(16) << min_value.second << ")";
        manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);
    }
    return std::make_tuple(min_value, min_transformation);
}

//　move値の正規化処理　結局必要になってしまった ここにpassがいくことはある　noneはないはずなのでエラー出力して落とそう
int normalize_move(int move, TransformId transformation, PositionManager& manager) {
    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        manager.debug_log("Normalizing move: " + std::to_string(move) + ", using transformation: " + transform_name(transformation), PositionManager::LogLevel::DEBUG);
    }

    if (move == 64) {  // パス
        manager.debug_log("At normalize move is a pass, returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }
    else if (move == 65) {  // 無効な手
        manager.debug_log("Invalid move (None) detected. Terminating program.", PositionManager::LogLevel::ERROR);
        std::exit(1);  // プログラムを終了
    }
    switch (transformation) {
    case TransformId::identity: return move;
    case TransformId::rotate_90: return rotate_move_90(move);
    case TransformId::rotate_180: return rotate_move_180(move);
    case TransformId::rotate_270: return rotate_move_270(move);
    case TransformId::flip_vertical: return flip_move_vertical(move);
    case TransformId::flip_horizontal: return flip_move_horizontal(move);
    case TransformId::flip_diag_a1h8: return flip_move_diag_a1h8(move);
    case TransformId::flip_diag_a8h1: return flip_move_diag_a8h1(move);
    default:
        manager.debug_log("Unknown board transformation for move normalization.", PositionManager::LogLevel::ERROR);
        std::exit(1);
    }
}

//　非正規化があるのはmove値のみ ここにnoneが行くことはあるので落としてはいけない。全消し時とか
int denormalize_move(int move, TransformId transformation, PositionManager& manager) {
    if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
        manager.debug_log("Denormalizing move: " + std::to_string(move) + ", using transformation: " + transform_name(transformation), PositionManager::LogLevel::DEBUG);
    }

    if (move == 64) {
        manager.debug_log("At denormalize move is a pass, returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }
    else if (move == 65) {
        manager.debug_log("Move is invalid (none), returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }

    switch (transformation) {
    case TransformId::identity: return move;
    case TransformId::rotate_90: return rotate_move_270(move);
    case TransformId::rotate_180: return rotate_move_180(move);
    case TransformId::rotate_270: return rotate_move_90(move);
    case TransformId::flip_vertical: return flip_move_vertical(move);
    case TransformId::flip_horizontal: return flip_move_horizontal(move);
    case TransformId::flip_diag_a1h8: return flip_move_diag_a1h8(move);
    case TransformId::flip_diag_a8h1: return flip_move_diag_a8h1(move);
    default:
        manager.debug_log("Unknown board transformation for move denormalization.", PositionManager::LogLevel::ERROR);
        std::exit(1);
    }
}

//　move値変換関数
inline int rotate_move_90(int move) {
    return (move % 8) * 8 + (7 - move / 8);
}

inline int rotate_move_270(int move) {
    return (7 - move % 8) * 8 + move / 8;
}

inline int flip_move_vertical(int move) {
    return (7 - move / 8) * 8 + move % 8;
}

inline int flip_move_horizontal(int move) {
    return (move / 8) * 8 + (7 - move % 8);
}

inline int flip_move_diag_a1h8(int move) {
    return (move % 8) * 8 + (move / 8);
}

inline int flip_move_diag_a8h1(int move) {
    return (7 - move % 8) * 8 + (7 - move / 8);
}

//　bookを読む関数はこんなところに
BookPosition* read_position(uint64_t my_stones, uint64_t opponent_stones) {
    auto it = book_positions.find(std::make_pair(my_stones, opponent_stones));
    return (it != book_positions.end()) ? &(it->second) : nullptr;
}

// 対称変換名や文字列を作らずに、盤面の正規化キーを求める。
inline std::pair<uint64_t, uint64_t> normalize_position_key_fast(uint64_t my_stones, uint64_t opponent_stones) {
    uint64_t min_my_stones = my_stones;
    uint64_t min_opponent_stones = opponent_stones;

    const auto& transforms = get_board_transforms();
    // identityはmin値の初期値として既に評価済みなので再計算しない。
    for (size_t i = 1; i < transforms.size(); ++i) {
        const auto& candidate = transforms[i];
        const uint64_t transformed_my_stones = candidate.transform(my_stones);
        const uint64_t transformed_opponent_stones = candidate.transform(opponent_stones);
        if (transformed_my_stones < min_my_stones ||
            (transformed_my_stones == min_my_stones && transformed_opponent_stones < min_opponent_stones)) {
            min_my_stones = transformed_my_stones;
            min_opponent_stones = transformed_opponent_stones;
        }
    }
    return { min_my_stones, min_opponent_stones };
}

// Mode 6/7 の大量probe用。std::stringやdebug logを生成せず直接検索する。
inline const BookPosition* read_normalized_book_position_fast(uint64_t my_stones, uint64_t opponent_stones) {
    const auto key = normalize_position_key_fast(my_stones, opponent_stones);
    return read_position(key.first, key.second);
}

// Mode 6/7 の probe 専用。move string / Position / manager.current_position を作らず、
// bitboard だけから「着手後に次の手番から見た盤面」を生成する。
// 64bit legal-move mask の最下位bit位置を取得する。
// Visual Studio では _BitScanForward64 を使い、mode 6/7 の「合法手が少数なのに64マス全走査」
// を避ける。非MSVC環境では __builtin_ctzll を使う。引数0は呼び出し側で禁止する。
inline unsigned int bit_scan_forward_64(uint64_t value) {
#ifdef _MSC_VER
    unsigned long index;
    _BitScanForward64(&index, value);
    return static_cast<unsigned int>(index);
#else
    return static_cast<unsigned int>(__builtin_ctzll(value));
#endif
}

struct BoardState {
    uint64_t my_stones;
    uint64_t opponent_stones;
};

inline BoardState apply_move_to_board(uint64_t my_stones, uint64_t opponent_stones, uint8_t move) {
    // Pass は石を動かさず、手番だけが入れ替わる。
    if (move == 64) {
        return BoardState{opponent_stones, my_stones};
    }

    // 既存 flip_stones() と同じ move 値 -> bit 位置の対応を使用する。
    const uint64_t move_bit = 1ULL << (63 - move);
    const uint64_t flipped = flip_all_directions(my_stones, opponent_stones, move_bit);

    // 次の Position は次の手番側から見た形式で返す。
    return BoardState{
        opponent_stones ^ flipped,
        my_stones | move_bit | flipped
    };
}

inline BoardState apply_move_to_board(const Position& position, uint8_t move) {
    return apply_move_to_board(position.my_stones, position.opponent_stones, move);
}

// Bookのリンクおよび、遷移先もBookに存在するleafをたどり、初期局面から
// 到達できる最深positionのplyをメモ化して求める。追加メモリは各BookPositionの
// 1 byteキャッシュだけで、深さ判定用の巨大な別hash mapは作らない。
int calculate_reachable_book_depth_for_key(const std::pair<uint64_t, uint64_t>& key,
    PositionManager& manager, size_t& positions_visited) {
    auto node_it = book_positions.find(key);
    if (node_it == book_positions.end()) return 0;

    BookPosition& node = node_it->second;
    if (node.auto_depth_cache == 0xFE) {
        manager.debug_log("Cycle detected while calculating book depth; cyclic edge ignored.",
            PositionManager::LogLevel::WARNING);
        return 0;
    }
    if (node.auto_depth_cache != 0xFF) return node.auto_depth_cache;

    node.auto_depth_cache = 0xFE;
    ++positions_visited;
    int deepest_child_depth = 0;

    auto consider_move = [&](uint8_t move) {
        if (move > 64) return;
        const BoardState child = apply_move_to_board(key.first, key.second, move);
        const auto child_key = normalize_position_key_fast(child.my_stones, child.opponent_stones);
        if (book_positions.find(child_key) == book_positions.end()) return;
        const int candidate_depth = 1 + calculate_reachable_book_depth_for_key(
            child_key, manager, positions_visited);
        if (candidate_depth > deepest_child_depth) deepest_child_depth = candidate_depth;
    };

    for (const Link& link : book_links(node)) consider_move(link.move);
    if (has_real_leaf(node.leaf)) consider_move(node.leaf.move);

    if (deepest_child_depth >= 0xFE) deepest_child_depth = 0xFD;
    node.auto_depth_cache = static_cast<uint8_t>(deepest_child_depth);
    return deepest_child_depth;
}

int calculate_reachable_book_depth(uint64_t my_stones, uint64_t opponent_stones,
    PositionManager& manager, size_t& positions_visited) {
    const auto root_key = normalize_position_key_fast(my_stones, opponent_stones);
    return calculate_reachable_book_depth_for_key(root_key, manager, positions_visited);
}

// 主にデバッグ用 mode5で動作。特定のポジション情報をbookから読み込んでdebuglogに表示するだけ
void read_specified_positions(const std::string& input_file_path, PositionManager& manager) {
    std::ifstream input_file(input_file_path);
    if (!input_file.is_open()) {
        manager.debug_log("Failed to open input file: " + input_file_path, PositionManager::LogLevel::ERROR);
        return;
    }
    // ファイルの読み込み
    std::string line;
    while (std::getline(input_file, line)) {
        std::istringstream iss(line);
        std::string my_position_str, opponent_position_str;

        if (!(iss >> my_position_str >> opponent_position_str)) {
            manager.debug_log("Invalid line format: " + line, PositionManager::LogLevel::ERROR);
            continue;
        }
        // 各行ごとに盤面情報の取得
        uint64_t my_position, opponent_position;
        try {
            my_position = std::stoull(my_position_str, nullptr, 16);
            opponent_position = std::stoull(opponent_position_str, nullptr, 16);
        }
        catch (const std::exception& e) {
            manager.debug_log("Error parsing hex values: " + line + " - " + e.what(), PositionManager::LogLevel::ERROR);
            continue;
        }
        // bookから読んでくる
        const BookPosition* position = read_position(my_position, opponent_position);
        if (position) {
            std::stringstream ss;
            ss << "Position found - My stones: " << my_position_str
                << ", Opponent stones: " << opponent_position_str
                << "\n" << format_book_position(my_position, opponent_position, *position);
            manager.debug_log(ss.str(), PositionManager::LogLevel::ERROR);
        }
        else {
            std::stringstream ss;
            ss << "Position not found - My stones: " << my_position_str
                << ", Opponent stones: " << opponent_position_str;
            manager.debug_log(ss.str(), PositionManager::LogLevel::ERROR);
        }
    }

    input_file.close();
}

// ================================================================
// Mode 6 / 7：book position 欠落検出用の補助関数
// ================================================================

bool has_unvisited_book_move(const Position& position) {
    // link がまだ未訪問なら探索を継続する。
    for (const auto& link : position.links) {
        if (!link.visited) {
            return true;
        }
    }

    // 既存 get_children() と同じ条件で「実在する leaf」のみを対象にする。
    const bool valid_leaf = has_real_leaf(position.leaf);

    return valid_leaf && !position.leaf.visited;
}

// Mode 6/7 用の棋譜生成。move -> 座標変換を行うが、
// 探索状態を持つ manager.current_kifu を変更しない。Pass は既存 main_process() の出力仕様と同じく省略する。
std::string append_move_to_kifu(const std::string& kifu, int move) {
    if (move == 64) {
        return kifu;
    }
    if (move == 65) {
        return kifu;
    }

    const char col = static_cast<char>('a' + (move % 8));
    const int row = (move / 8) + 1;

    std::string result;
    result.reserve(kifu.size() + 2);
    result = kifu;
    result.push_back(col);
    result.push_back(static_cast<char>('0' + row));
    return result;
}

// Mode 6/7 の検出結果を既存 mismatch_process() と同じ「1行1棋譜」形式で追記する。
// mode 6: legal move 後まで出力。
// mode 7: legal move 直前まで出力。
void output_mode6_7_kifu(const std::string& kifu, ResultWriter& output, PositionManager& manager, std::set<std::string>& reported_kifus, int mode) {
    if (!reported_kifus.insert(kifu).second) {
        return;
    }

    output.append_line(kifu);
    if (manager.is_log_enabled(PositionManager::LogLevel::INFO)) {
        manager.debug_log("Mode " + std::to_string(mode) + ": Missing position pattern found. Kifu: " + kifu,
            PositionManager::LogLevel::INFO);
    }
}

// Mode 6/7 の核心部分。
// 1. 現在の book leaf の手を bitboard で指して child position を生成。
// 2. その child position が book にあれば正常なので除外。
// 3. book に無ければ child position の全合法手を列挙。
// 4. 合法手後の position が book にある手を検出。
// 5. Mode 6 は合法手まで、Mode 7 は合法手直前までの棋譜を出力する。
//
// 元コードとの差分として、probe の途中では create_position_data() / normalize_position()
// を使わず、bitboard 直接生成 + 変換名を作らない高速 lookup を使用する。これにより mode 6/7 の大量 probe の一時オブジェクトと文字列生成を削減する。
void scan_missing_leaf_mode6_7(const Position& leaf_parent, const std::string& kifu, ResultWriter& output,
    PositionManager& manager, std::set<std::string>& reported_kifus, const SearchSettings& settings) {

    const bool valid_leaf = has_real_leaf(leaf_parent.leaf);

    if (!valid_leaf) {
        return;
    }

    const uint8_t leaf_move = leaf_parent.leaf.move;

    // leaf 着手後の盤面は bitboard のみで生成する。
    const BoardState leaf_child = apply_move_to_board(leaf_parent, leaf_move);

    // leaf 直後の position が book に存在するなら position 欠落ではない。
    if (read_normalized_book_position_fast(leaf_child.my_stones, leaf_child.opponent_stones)) {
        return;
    }

    // Mode 7 の出力基準となる「leaf 後・reply 前」の棋譜を一度だけ作る。
    const std::string kifu_after_leaf = append_move_to_kifu(kifu, leaf_move);

    // 欠落 position の現在手番から見た合法手を直接生成する。
    const uint64_t legal_move_mask = generate_legal_moves(leaf_child.my_stones, leaf_child.opponent_stones);

    if (legal_move_mask != 0) {
        // legal_move_mask の立っているbitだけを順番に取り出す。
        // 従来の64マス全走査より、合法手が少ない通常の局面では大幅に無駄が減る。
        uint64_t remaining_moves = legal_move_mask;
        while (remaining_moves != 0) {
            const unsigned int bit_index = bit_scan_forward_64(remaining_moves);
            const int move = 63 - static_cast<int>(bit_index);
            remaining_moves &= (remaining_moves - 1);

            // 合法手後の position を直接生成し、正規化キーだけで book を検索する。
            const BoardState reply_child = apply_move_to_board(leaf_child.my_stones, leaf_child.opponent_stones,
                static_cast<uint8_t>(move));

            if (!read_normalized_book_position_fast(reply_child.my_stones, reply_child.opponent_stones)) {
                continue;
            }

            // Mode 6 は reply 後、Mode 7 は reply 直前を出力する。
            const std::string output_kifu =
                (settings.mode == 6)
                ? append_move_to_kifu(kifu_after_leaf, move)
                : kifu_after_leaf;

            output_mode6_7_kifu(output_kifu, output, manager, reported_kifus, settings.mode);
        }
    }
    else {
        // 配置手が一つも無い場合、相手に合法手があれば Pass が合法。
        const BoardState opponent_turn{leaf_child.opponent_stones, leaf_child.my_stones};
        if (generate_legal_moves(opponent_turn.my_stones, opponent_turn.opponent_stones) == 0) {
            // 両者とも合法手がない終了局面では probe 対象となる次の position が存在しないため何もしない。
            return;
        }

        // Pass 後の position を直接生成し、book に存在するか確認する。
        const BoardState pass_child = apply_move_to_board(leaf_child.my_stones, leaf_child.opponent_stones, 64);
        if (read_normalized_book_position_fast(pass_child.my_stones, pass_child.opponent_stones)) {
            // Pass は棋譜文字列に含めないので、Mode 6/7 とも kifu_after_leaf をそのまま出力する。
            output_mode6_7_kifu(kifu_after_leaf, output, manager, reported_kifus, settings.mode);
        }
    }
}

// mode 1～4 と mode 6～7 の DFS を統合した共通探索関数。
// 共通部分（visited管理、child生成、誤差制限、手数制限、再帰）を1か所にまとめ、
// mode 6/7 だけに必要な「欠落 leaf 検査」を child_not_found 時の分岐として実行する。
void main_process_recursive(Position& current_position, std::string& current_kifu, ResultWriter& output,
    PositionManager& manager, const SearchSettings& settings, std::set<std::string>& reported_kifus,
    int move_number, const ErrorState& error_state) {

    // Mode 5 以外の最大Book ply制限。
    // 現在の Position から次のBook moveを指すと1 ply深くなるため、
    // 現在のplyが上限以上なら、このPositionから先は探索しない。
    if (!can_search_next_book_move(move_number, settings.max_book_move_depth)) {
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Mode " + std::to_string(settings.mode) + ": Maximum book move depth reached. Search branch stopped. Current plies=" +
                std::to_string(move_number) +
                ", max_book_move_depth=" + std::to_string(settings.max_book_move_depth), PositionManager::LogLevel::DEBUG);
        }
        return;
    }

    // mode 1～4 / 6～7 で共通のループ進捗表示。
    update_search_progress(manager);

    while (true) {
        if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
            manager.debug_log("Current position: " + format_position(current_position), PositionManager::LogLevel::DEBUG);
            manager.debug_log("Current kifu: " + current_kifu, PositionManager::LogLevel::DEBUG);
        }

        // 「今回の get_children() 呼び出しで leaf が初めて訪問されたか」を保存する。
        const bool leaf_visited_before = current_position.leaf.visited;

        const std::size_t parent_kifu_size = current_kifu.size();
        Position child_position;
        TransformId transformation;
        uint8_t move;
        std::tie(child_position, transformation, move) = get_children(manager, current_position, current_kifu);

        if (transformation == TransformId::child_not_found) {
            if (settings.missing_position_mode()) {
                // Mode 6/7 は leaf の child が book に無いケース自体を検出対象とする。
                if (!leaf_visited_before && current_position.leaf.visited && has_real_leaf(current_position.leaf)) {
                    ErrorState state_after_leaf;
                    int leaf_move_loss = 0;
                    if (settings.error_limits_enabled()) {
                        if (try_apply_error_limits(current_position, current_position.leaf.move, current_position.leaf.eval,
                            error_state, settings.per_move_error, settings.cumulative_error, state_after_leaf, leaf_move_loss)) {
                            scan_missing_leaf_mode6_7(current_position, current_kifu, output, manager,
                                reported_kifus, settings);
                        }
                        else {
                            log_error_pruned_move(manager, current_position.leaf.move, leaf_move_loss, error_state,
                                settings.per_move_error, settings.cumulative_error);
                        }
                    }
                    else {
                        state_after_leaf = error_state;
                        state_after_leaf.black_to_move = !error_state.black_to_move;
                        scan_missing_leaf_mode6_7(current_position, current_kifu, output, manager,
                            reported_kifus, settings);
                    }
                }
            }

            // A missing child ends only that edge. Keep processing remaining sibling links/leaves.
            if (has_unvisited_book_move(current_position)) continue;
            break;
        }

        // mode 1～4 / 6～7 共通の誤差制限。
        ErrorState next_error_state;
        int move_loss = 0;
        if (settings.error_limits_enabled()) {
            const int8_t move_eval = calculate_parent_eval(current_position, move, manager);
            if (!try_apply_error_limits(current_position, move, move_eval, error_state,
                settings.per_move_error, settings.cumulative_error, next_error_state, move_loss)) {
                log_error_pruned_move(manager, move, move_loss, error_state,
                    settings.per_move_error, settings.cumulative_error);
                current_kifu.resize(parent_kifu_size);
                continue;
            }
        }
        else {
            next_error_state = error_state;
            next_error_state.black_to_move = !error_state.black_to_move;
        }

        // 既存仕様どおり、Pass は通常の探索棋譜から除去する。
        if (move == 64) {
            current_kifu.resize(parent_kifu_size);
            if (manager.is_log_enabled(PositionManager::LogLevel::DEBUG)) {
                manager.debug_log("Pass detected and removed from new_kifu, updated kifu: " + current_kifu, PositionManager::LogLevel::DEBUG);
            }
        }

        const int next_move_number = move_number + 1;

        if (settings.missing_position_mode()) {
            // Mode 6/7 は mismatch 判定ではなく、leaf 欠落検出を目的とするため child position へ進むだけ。
        }
        else if (can_search_next_book_move(next_move_number, settings.max_book_move_depth)) {
            // Mode 1～4 の従来処理。
            const bool mismatch = judge_mismatch(child_position, current_position, move, settings.mode, manager);
            if (mismatch) {
                // Mode 3/4 candidate selection must use the selected edge's score,
                // matching judge_mismatch(), rather than the parent's position value.
                const int8_t parent_move_eval = calculate_parent_eval(current_position, move, manager);
                mismatch_process(child_position, current_kifu, output, manager,
                    child_position.eval_value, parent_move_eval, settings.mode,
                    settings.per_move_error, settings.cumulative_error, next_error_state);
            }
        }

        // child position の手数が max_book_move_depth に到達した場合、その先の leaf/link は探索しない。
        if (!can_search_next_book_move(next_move_number, settings.max_book_move_depth)) {
            current_kifu.resize(parent_kifu_size);
            continue;
        }

        main_process_recursive(child_position, current_kifu, output, manager, settings,
            reported_kifus, next_move_number, next_error_state);
        current_kifu.resize(parent_kifu_size);
    }
}

// Tests and small callers may pass a temporary/const base string. Copy it once at
// the entry point; the recursive implementation then reuses that buffer in place.
void main_process_recursive(Position& current_position, const std::string& current_kifu, ResultWriter& output,
    PositionManager& manager, const SearchSettings& settings, std::set<std::string>& reported_kifus,
    int move_number, const ErrorState& error_state) {
    std::string reusable_kifu = current_kifu;
    reusable_kifu.reserve(std::max<std::size_t>(reusable_kifu.capacity(), 128));
    main_process_recursive(current_position, reusable_kifu, output, manager, settings,
        reported_kifus, move_number, error_state);
}

// mode 1～4 / 6～7 共通の開始処理。
// Mode 5 は指定 position 読み込み専用なので従来どおり別処理とする。
void main_process(const std::string& output_path, PositionManager& manager, const SearchSettings& settings) {
    try {
        manager.program_start_time = std::chrono::steady_clock::now();

        // 既存 main_process() と同じ初期局面を使用する。
        constexpr uint64_t initial_my_stones = 0x0000000810000000ULL;
        constexpr uint64_t initial_opponent_stones = 0x0000001008000000ULL;
        BookPosition* initial_book_position = read_position(initial_my_stones, initial_opponent_stones);
        if (!initial_book_position) {
            manager.debug_log("Initial position not found in book. Terminating program.", PositionManager::LogLevel::ERROR);
            std::exit(1);
        }

        Position initial_position;
        initial_position.my_stones = initial_my_stones;
        initial_position.opponent_stones = initial_opponent_stones;
        const auto initial_links = book_links(*initial_book_position);
        initial_position.links.assign(initial_links.begin(), initial_links.end());
        initial_position.leaf = initial_book_position->leaf;
        initial_position.eval_value = initial_book_position->eval_value;
        initial_position.book_record = initial_book_position;
        ResultWriter result_output(output_path, manager);

        // Mode 6/7 では重複棋譜防止に使用する。Mode 1～4 では空のままなので、
        // ノードごとのメモリ負荷は発生しない。
        std::set<std::string> reported_kifus;
        const ErrorState initial_error_state{};

        std::string initial_kifu;
        initial_kifu.reserve(128);
        main_process_recursive(initial_position, initial_kifu, result_output, manager,
            settings, reported_kifus, 0, initial_error_state);

        std::cout << "\r" << manager.loop_count << " Links or Leaf processed (Final)" << std::endl;
        manager.debug_log("Total Links or Leaf processed (Mode " + std::to_string(settings.mode) + "): " +
            std::to_string(manager.loop_count), PositionManager::LogLevel::WARNING);

        if (settings.missing_position_mode()) {
            manager.debug_log("Mode " + std::to_string(settings.mode) + " reported unique kifu count: " +
                std::to_string(reported_kifus.size()), PositionManager::LogLevel::INFO);
        }

        const auto program_end_time = std::chrono::steady_clock::now();
        const std::chrono::duration<double> program_duration = program_end_time - manager.program_start_time;
        manager.debug_log("Total program execution time (Mode " + std::to_string(settings.mode) + "): " +
            std::to_string(program_duration.count()) + " seconds", PositionManager::LogLevel::WARNING);
        std::cout << "Total program execution time: " << program_duration.count() << " seconds" << std::endl;
    }
    catch (const std::exception& e) {
        manager.debug_log("Exception in main_process: " + std::string(e.what()), PositionManager::LogLevel::ERROR);
        std::cerr << "Critical error in main_process: " << e.what() << std::endl;
        std::exit(1);
    }
}

// メイン関数　ファイルパスの指定とコンフィグ読み込み→book読み込み→メイン関数読み込み
int main() {
    std::string book_path = "book.dat";
    std::string debug_log_path = "debuglog.txt";
    std::string output_path = "mismatched_positions.txt";
    std::string config_path = "config.ini";
    std::string specified_positions_path = "specified_positions.txt";

    try {
        auto [log_level, auto_adjust, adjusted_level, mode, per_move_error, cumulative_error,
            max_book_move_depth, auto_detect_book_depth, progress_update_interval] = read_config(config_path);

        if (progress_update_interval <= 0) {
            std::cerr << "Error: progress_update_interval must be a positive integer." << std::endl;
            return 1;
        }

        PositionManager manager(book_path, debug_log_path, log_level, auto_adjust, adjusted_level,
            static_cast<size_t>(progress_update_interval));

        manager.debug_log("per_move_error = " + std::to_string(per_move_error) +
            ", cumulative_error = " + std::to_string(cumulative_error) +
            " (-1 means unrestricted)", PositionManager::LogLevel::INFO);
        manager.debug_log("auto_detect_book_depth = " + std::string(auto_detect_book_depth ? "True" : "False"),
            PositionManager::LogLevel::INFO);
        manager.debug_log("max_book_move_depth = " + std::to_string(max_book_move_depth) +
            (auto_detect_book_depth ? " (ignored because auto detection is enabled)" : " (-1 means unrestricted)"),
            PositionManager::LogLevel::INFO);
        manager.debug_log("progress_update_interval = " + std::to_string(progress_update_interval),
            PositionManager::LogLevel::INFO);

        if (mode < 1 || mode > 7) {
            std::cerr << "Error: Invalid mode (" << mode << "). Mode must be between 1 and 7." << std::endl;
            manager.debug_log("Invalid mode: " + std::to_string(mode), PositionManager::LogLevel::ERROR);
            return 1;
        }

        // 1手あたりの誤差・積算誤差は -1 が無制限、それ以外は0以上の整数とする。
        if (mode != 5 && (per_move_error < -1 || cumulative_error < -1)) {
            std::cerr << "Error: per_move_error and cumulative_error must be -1 or a non-negative integer." << std::endl;
            manager.debug_log("Invalid error limits: per_move_error=" + std::to_string(per_move_error) +
                ", cumulative_error=" + std::to_string(cumulative_error), PositionManager::LogLevel::ERROR);
            return 1;
        }

        // 手数上限は -1 が無制限、0以上が有効。自動判定時は手動値を検証・使用しない。
        if (mode != 5 && !auto_detect_book_depth && max_book_move_depth < -1) {
            std::cerr << "Error: max_book_move_depth must be -1 or a non-negative integer." << std::endl;
            manager.debug_log("Invalid max_book_move_depth: " + std::to_string(max_book_move_depth), PositionManager::LogLevel::ERROR);
            return 1;
        }

        load_all_positions(book_path, manager);

        if (mode != 5 && auto_detect_book_depth) {
            constexpr uint64_t initial_my_stones = 0x0000000810000000ULL;
            constexpr uint64_t initial_opponent_stones = 0x0000001008000000ULL;
            size_t depth_positions_visited = 0;
            const int deepest_position_ply = calculate_reachable_book_depth(
                initial_my_stones, initial_opponent_stones, manager, depth_positions_visited);
            if (!read_normalized_book_position_fast(initial_my_stones, initial_opponent_stones)) {
                std::cerr << "Error: Initial position not found in book; cannot auto-detect book depth." << std::endl;
                manager.debug_log("Initial position not found; automatic book-depth detection failed.",
                    PositionManager::LogLevel::ERROR);
                return 1;
            }
            // The DFS cutoff is exclusive: max=D+1 processes book positions through ply D.
            // Modes 1-4 must judge a destination at ply D, while Modes 6/7 must inspect
            // the leaf of a deepest position at D; its following reply record can be
            // separated by a missing position and therefore is not included in this DFS depth.
            max_book_move_depth = deepest_position_ply + 1;
            std::cout << "Auto-detected book depth: " << deepest_position_ply
                << " plies (deepest reachable book position)." << std::endl;
            std::cout << "Search will inspect positions through ply " << deepest_position_ply
                << " (exclusive cutoff " << max_book_move_depth << ")." << std::endl;
            manager.debug_log("Auto-detected deepest reachable book position: " +
                std::to_string(deepest_position_ply) + " plies; effective max_book_move_depth=" +
                std::to_string(max_book_move_depth) + "; positions visited=" +
                std::to_string(depth_positions_visited), PositionManager::LogLevel::INFO);
        }

        switch (mode) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 6:
        case 7: {
            // Mode 1～4 と Mode 6～7 は共通 DFS を使用する。
            // Mode 6/7 だけ settings.missing_position_mode() が true になり、欠落 leaf 検査へ分岐する。
            const SearchSettings settings{
                mode,
                per_move_error,
                cumulative_error,
                max_book_move_depth
            };
            main_process(output_path, manager, settings);
            break;
        }
        case 5:
            // Mode 5 は指定 position の読み込み専用なので、探索系の共通 DFS には入れない。
            read_specified_positions(specified_positions_path, manager);
            break;
        }
    }
    catch (const std::exception& e) {
        // エラーメッセージをデバッグログに出力
        PositionManager manager("", "debuglog.txt");  // 一時的なmanagerオブジェクトを作成
        manager.debug_log("Critical error in main: " + std::string(e.what()), PositionManager::LogLevel::ERROR);
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
