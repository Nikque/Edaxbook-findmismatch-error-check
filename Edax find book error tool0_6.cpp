#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <tuple>
#include <set>
#include <stack>
#include <chrono>
#include <functional>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <filesystem>


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

// ハッシュ関数 結局これが一番うまくいきそうなのなんで
struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

// 等価比較演算子の定義
struct PairEqual {
    template <class T1, class T2>
    bool operator()(const std::pair<T1, T2>& lhs, const std::pair<T1, T2>& rhs) const {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }
};

struct Position {
    uint64_t my_stones = 0;
    uint64_t opponent_stones = 0;
    std::vector<Link> links;
    Leaf leaf = { 0, 0, false };
    int8_t eval_value = 0;
};

// unorderd map 本体
using PositionMap = std::unordered_map<std::pair<uint64_t, uint64_t>, Position, PairHash, PairEqual>;

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

    // ポジションマネージャーの変数宣言部分
    std::string book_path;
    std::string debug_log_path;
    Position current_position;
    std::string current_kifu;
    mutable LogLevel log_level;
    bool auto_adjust_log_level;
    LogLevel adjusted_log_level;

    // 初期設定等
    PositionManager(const std::string& book_path, const std::string& debug_log_path,
        LogLevel level = LogLevel::ERROR,
        bool auto_adjust = false,
        LogLevel adjusted_level = LogLevel::INFO)
        : book_path(book_path), debug_log_path(debug_log_path),
        log_level(level), current_kifu(""),
        auto_adjust_log_level(auto_adjust),
        adjusted_log_level(adjusted_level) {
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

// config.ini 読み込み関数を修正
std::tuple<PositionManager::LogLevel, bool, PositionManager::LogLevel, int> read_config(const std::string& config_path) {
    // 設定ファイルを開く
    std::ifstream config_file(config_path);
    std::string line;

    // デフォルト値の設定
    PositionManager::LogLevel log_level = PositionManager::LogLevel::ERROR;
    bool auto_adjust = false;
    PositionManager::LogLevel adjusted_level = PositionManager::LogLevel::INFO;
    int mode = 4;  // デフォルトモードを4に設定

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
    }

    // 返値: ログレベル、自動調整フラグ、調整後のログレベル、モードのタプル
    return std::make_tuple(log_level, auto_adjust, adjusted_level, mode);
}

// move値の実際の実装が説明と異なる部分があるための修正用
inline uint8_t rotate_move_180(uint8_t move) {
    if (move >= 64) {
        return move;
    }
    return 63 - move;
}

// unorderd_map用のハッシュ関数の衝突回数を調べる　ハッシュ領域が足りてないのでどんなハッシュ関数を用意しても衝突率33％はある
size_t count_collisions(const PositionMap& map) {
    std::vector<size_t> bucket_sizes(map.bucket_count());
    for (size_t i = 0; i < map.bucket_count(); ++i) {
        bucket_sizes[i] = map.bucket_size(i);
    }

    size_t collisions = 0;
    for (size_t size : bucket_sizes) {
        if (size > 1) {
            collisions += size - 1;
        }
    }
    // 返値 衝突回数
    return collisions;
}

// bookデータをbook posiitons unorderd mapへ全てぶち込む　いらないデータは捨てる
void load_all_positions(const std::string& book_path, PositionManager& manager) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // ファイルを開く
    FILE* fp = nullptr;
    errno_t err = fopen_s(&fp, book_path.c_str(), "rb");
    if (err != 0 || fp == nullptr) {
        manager.debug_log("Failed to open book file: " + book_path, PositionManager::LogLevel::ERROR);
        return;
    }

    // ファイルサイズを取得
    _fseeki64(fp, 0, SEEK_END);
    std::uintmax_t filesize = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    manager.debug_log("File size: " + std::to_string(filesize) + " bytes", PositionManager::LogLevel::INFO);

    // ポジション数を推定
    constexpr double avg_position_size = 44.0720;
    double estimated_positions_double = static_cast<double>(filesize) / avg_position_size;
    size_t estimated_positions = static_cast<size_t>(estimated_positions_double);

    // 負荷係数を考慮してバケット数を計算
    size_t estimated_buckets = static_cast<size_t>(estimated_positions * 1.10);
    
    // reserveの前にバケット数を出力　そしてreserve
    manager.debug_log("Estimated number of buckets: " + std::to_string(estimated_buckets), PositionManager::LogLevel::DEBUG);
    book_positions.reserve(estimated_buckets);

    if (manager.log_level == PositionManager::LogLevel::DEBUG) {
        manager.debug_log("Actual bucket count after reserve: " + std::to_string(book_positions.bucket_count()), PositionManager::LogLevel::DEBUG);
        manager.debug_log("Estimated number of positions: " + std::to_string(estimated_positions), PositionManager::LogLevel::DEBUG);

        // バケットのメモリ使用量（ポインタサイズ）
        size_t bucket_memory = book_positions.bucket_count() * sizeof(void*);

        // 要素のメモリ使用量
        constexpr double bytes_per_position = 47.0;  // Position構造体の平均サイズ
        double element_memory = estimated_positions * bytes_per_position;

        // 合計推定メモリ使用量
        double total_estimated_memory_mb = (bucket_memory + element_memory) / (1048576);
        manager.debug_log("Estimated total memory usage: " + std::to_string(total_estimated_memory_mb) + " MB", PositionManager::LogLevel::DEBUG);
    }

    // ヘッダーをスキップ
    fseek(fp, 42, SEEK_SET);

    // 読み込み時間の測定
    auto read_start_time = std::chrono::high_resolution_clock::now();

    // 変数の初期化
    size_t positions_loaded = 0;

    while (true) {
        uint64_t my_stones = 0, opponent_stones = 0;
        int16_t raw_value = 0;
        uint8_t numberline = 0;
        int8_t value = 0;

        // 変数の読み込み
        if (fread(&my_stones, sizeof(my_stones), 1, fp) != 1) break;
        if (fread(&opponent_stones, sizeof(opponent_stones), 1, fp) != 1) break;
        fseek(fp, 16, SEEK_CUR);  // win, draw, lose, lineをスキップ
        if (fread(&raw_value, sizeof(raw_value), 1, fp) != 1) break;
        fseek(fp, 4, SEEK_CUR);  // minvalue, maxvalueをスキップ
        if (fread(&numberline, sizeof(numberline), 1, fp) != 1) break;
        fseek(fp, 1, SEEK_CUR);  // levelをスキップ

        // 評価値が範囲外だった場合
        if (raw_value < -127 || raw_value > 127) {
            manager.debug_log("Error: Value out of int8_t range: " + std::to_string(raw_value), PositionManager::LogLevel::ERROR);
            std::exit(1);
        }
        value = static_cast<int8_t>(raw_value);

        // リンクとリーフの処理
        std::vector<Link> links;
        for (int i = 0; i < numberline; ++i) {
            int8_t link_value = 0;
            uint8_t link_move = 0;
            if (fread(&link_value, sizeof(link_value), 1, fp) != 1) break;
            if (fread(&link_move, sizeof(link_move), 1, fp) != 1) break;
            links.emplace_back(Link{rotate_move_180(link_move), link_value, false});
        }

        int8_t leaf_eval = 0;
        uint8_t leaf_move = 0;
        if (fread(&leaf_eval, sizeof(leaf_eval), 1, fp) != 1) break;
        if (fread(&leaf_move, sizeof(leaf_move), 1, fp) != 1) break;

        // ポジション構造体の作成
        Position position = {
            my_stones,
            opponent_stones,
            std::move(links),
            {rotate_move_180(leaf_move), leaf_eval, false},
            value
        };

        book_positions.emplace(std::make_pair(my_stones, opponent_stones), std::move(position));
        positions_loaded++;

        // 10万ポジションごとに進捗を表示
        if (positions_loaded % 100000 == 0) {
            std::cout << "\r" << positions_loaded << " Loading Completed" << std::flush;
        }
    }

    fclose(fp);

    // 最終的な読み込み数を表示
    std::cout << "\r" << positions_loaded << " Loading Completed" << std::endl;

    // 読み込み時間測定終了
    auto read_end_time = std::chrono::high_resolution_clock::now();
    auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(read_end_time - read_start_time);

    manager.debug_log("Actual number of positions loaded: " + std::to_string(positions_loaded), PositionManager::LogLevel::INFO);

    if (manager.log_level == PositionManager::LogLevel::DEBUG) {
        // unordered_mapのメモリ使用量を推定
        size_t bucket_memory_actual = book_positions.bucket_count() * sizeof(void*);

        // ノードサイズの計算（vectorのサイズを除外）
        size_t node_size = sizeof(std::pair<const std::pair<uint64_t, uint64_t>, Position>) - sizeof(std::vector<Link>);
        size_t aligned_node_size = (node_size + 15) & ~15;  // 16バイトアラインメント

        // ノードのメモリ（vectorを除く）
        size_t nodes_memory = aligned_node_size * book_positions.size();

        // linksのメモリ使用量を推定（vectorオブジェクト自体のサイズを含む）
        size_t total_links_memory = 0;
        size_t vector_size = sizeof(std::vector<Link>);
        for (const auto& pair : book_positions) {
            const Position& pos = pair.second;
            size_t links_memory = vector_size + (pos.links.capacity() * sizeof(Link));
            total_links_memory += links_memory;
        }

        // 総メモリ使用量
        size_t total_memory = bucket_memory_actual + nodes_memory + total_links_memory;

        // メモリ使用量をデバッグログに出力
        std::stringstream ss;
        ss << "Estimated memory usage of book_positions:"
            << "\n  Bucket memory: " << bucket_memory_actual << " bytes"
            << "\n  Node size (excluding vector): " << node_size << " bytes (aligned to " << aligned_node_size << " bytes)"
            << "\n  Nodes memory: " << nodes_memory << " bytes"
            << "\n  Links memory (including vector objects): " << total_links_memory << " bytes"
            << "\n  Total memory: " << total_memory << " bytes"
            << "\n  Total memory (MB): " << (total_memory / (1024.0 * 1024.0)) << " MB";
        manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);

        // Position構造体のサイズを出力
        ss.str("");
        ss << "Size of structures:"
            << "\n  Position: " << sizeof(Position) << " bytes"
            << "\n  Link: " << sizeof(Link) << " bytes"
            << "\n  Leaf: " << sizeof(Leaf) << " bytes"
            << "\n  std::vector<Link>: " << sizeof(std::vector<Link>) << " bytes";
        manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);

        // ハッシュ関数の衝突回数の測定
        size_t collisions = count_collisions(book_positions);
        manager.debug_log("Number of hash collisions: " + std::to_string(collisions), PositionManager::LogLevel::DEBUG);
        manager.debug_log("Collision rate: " + std::to_string(static_cast<double>(collisions) / positions_loaded), PositionManager::LogLevel::DEBUG);
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

// 各関数の宣言
std::tuple<Position, std::string, std::string, uint8_t> get_children(PositionManager& manager, Position& position);
std::tuple<Position, std::string, std::string> process_position(Position& position, const std::string& kifu, uint8_t move, PositionManager& manager);
std::tuple<Position, std::string> create_position_data(PositionManager& manager, int move = -1);
std::tuple<std::string, std::string> convert_move_to_str(int move, const std::string& kifu, PositionManager& manager);
Position flip_stones(const Position& position, const std::string& move_str, PositionManager& manager);
uint64_t shift(uint64_t bit, int direction);
std::tuple<std::tuple<uint64_t, uint64_t>, std::string> normalize_position(uint64_t my_stones, uint64_t opponent_stones, PositionManager& manager);
int denormalize_move(int move, const std::string& transformation_name, PositionManager& manager);
int rotate_move_90(int move);
int rotate_move_270(int move);
int flip_move_vertical(int move);
int flip_move_horizontal(int move);
int flip_move_diag_a1h8(int move);
int flip_move_diag_a8h1(int move);
int normalize_move(int move, const std::string& transformation_name, PositionManager& manager);
const Position* read_position(uint64_t my_stones, uint64_t opponent_stones);
void mismatch_process(const Position& child_position, const std::string& kifu, const std::string& transformation_name, const std::string& output_path, PositionManager& manager, int8_t child_eval, int8_t parent_eval, int mode);
int8_t calculate_parent_eval(const Position& parent_position, uint8_t move, PositionManager& manager);
void main_process_recursive(Position& current_position, std::string current_kifu, const std::string& output_path, PositionManager& manager, int mode);

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
        manager.debug_log("Found matching leaf - move: " + std::to_string(static_cast<int>(move)) +
            ", parent_eval: " + std::to_string(static_cast<int>(parent_eval)), PositionManager::LogLevel::INFO);
    }

    // 返値 親の評価値
    return parent_eval;
}

// ミスマッチ判定のための関数
bool judge_mismatch(const Position& child_position, const Position& parent_position, uint8_t move, int mode, PositionManager& manager) {
    int8_t child_eval = child_position.eval_value;
    bool mismatch = false;
    std::string comparison_details;

    if (mode == 3) {
        // Mode 3: 親ポジションの該当するリンクやリーフの手の評価値と子ポジションの評価値の反転を比較
        int8_t parent_eval = calculate_parent_eval(parent_position, move, manager);
        mismatch = parent_eval != -child_eval;
        comparison_details = "Mode 3: parent_eval (" + std::to_string(parent_eval) +
            ") vs -child_eval (" + std::to_string(-child_eval) + ")";
    }
    else {
        // リンクの最大評価値を計算（リーフを除く）
        int8_t max_child_link_eval = -64; // -64で初期化
        for (const auto& link : child_position.links) {
            if (link.eval_link > max_child_link_eval) {
                max_child_link_eval = link.eval_link;
            }
        }

        switch (mode) {
        case 1: {
            // Mode 1: リンクが存在し、子ポジションのリーフの評価値がリンクの最大評価値より大きい場合に不一致
            if (!child_position.links.empty()) {
                mismatch = child_position.leaf.eval > max_child_link_eval;
                comparison_details = "Mode 1: leaf_eval (" + std::to_string(child_position.leaf.eval) +
                    ") vs max_child_link_eval (" + std::to_string(max_child_link_eval) + ")";
            }
            else {
                // リンクが存在しない場合は不一致としない
                mismatch = false;
                comparison_details = "Mode 1: No links present, skipping mismatch check";
            }
            break;
        }
        case 2:
        case 4: {
            // Mode 2と4用のmax_child_move_evalを計算
            int8_t max_child_move_eval = max_child_link_eval;  // max_child_link_evalで初期化
            if (child_position.leaf.eval > max_child_move_eval) {
                max_child_move_eval = child_position.leaf.eval;
            }

            if (mode == 2) {
                // Mode 2: 子ポジションの評価値と最大評価値を比較
                mismatch = child_eval != max_child_move_eval;
                comparison_details = "Mode 2: child_eval (" + std::to_string(child_eval) +
                    ") vs max_child_move_eval (" + std::to_string(max_child_move_eval) + ")";
            }
            else { // mode == 4
                // Mode 4: 親ポジションの該当するリンクやリーフの手の評価値と子ポジションのリンクやリーフの内の最大評価値の反転を比較
                int8_t parent_eval = calculate_parent_eval(parent_position, move, manager);
                mismatch = parent_eval != -max_child_move_eval;
                comparison_details = "Mode 4: parent_eval (" + std::to_string(parent_eval) +
                    ") vs -max_child_move_eval (" + std::to_string(-max_child_move_eval) + ")";
            }
            break;
        }
        }
    }

    if (mismatch) {
        manager.debug_log("Mismatch detected: " + comparison_details, PositionManager::LogLevel::DEBUG);
    }
    else {
        manager.debug_log("No mismatch: " + comparison_details, PositionManager::LogLevel::DEBUG);
    }

    return mismatch;
}

// 不一致発見の場合の処理
void mismatch_process(const Position& child_position, const std::string& kifu, const std::string& transformation_name, const std::string& output_path, PositionManager& manager, int8_t child_eval, int8_t parent_eval, int mode) {
    std::vector<std::pair<std::string, uint8_t>> matching_moves;

    // 出力ファイルを開く
    std::ofstream output_file(output_path, std::ios::app | std::ios::binary);
    if (!output_file.is_open()) {
        manager.debug_log("Failed to open or create output file: " + output_path, PositionManager::LogLevel::ERROR);
        return;
    }

    // ファイルが新規作成された場合、BOMを書き込む
    output_file.seekp(0, std::ios::end);
    if (output_file.tellp() == 0) {
        output_file << static_cast<char>(0xEF) << static_cast<char>(0xBB) << static_cast<char>(0xBF);
    }

    if (mode == 1) {
        // Mode 1: 子ポジションのリーフまでの棋譜を出力
        std::string move_str, updated_kifu;
        std::tie(move_str, updated_kifu) = convert_move_to_str(child_position.leaf.move, kifu, manager);
        output_file << updated_kifu << std::endl;
        manager.debug_log("Mismatch found (Mode 1, leaf move). Kifu: " + updated_kifu + " (Move: " + std::to_string(child_position.leaf.move) + ")", PositionManager::LogLevel::DEBUG);
    }
    else {
        // max_child_move_evalの再計算
        int8_t max_child_move_eval = INT8_MIN;
        for (const auto& link : child_position.links) {
            if (link.eval_link > max_child_move_eval) {
                max_child_move_eval = link.eval_link;
            }
        }
        if (child_position.leaf.eval > max_child_move_eval) {
            max_child_move_eval = child_position.leaf.eval;
        }

        bool is_greater = false;
        int8_t comparison_value;

        // モードに応じて不一致の条件と比較値を設定
        switch (mode) {
        case 2:
            // Mode 2: 子ポジションの評価値と最大評価値を比較
            is_greater = max_child_move_eval > child_eval;
            comparison_value = -child_eval;
            break;
        case 3:
            // Mode 3: 親ポジションの該当するリンクやリーフの手の評価値と子ポジションの評価値の反転を比較
            is_greater = -child_eval > parent_eval;
            comparison_value = -parent_eval;
            break;
        case 4:
            // Mode 4: 親ポジションの該当するリンクやリーフの手の評価値と子ポジションのリンクやリーフの内の最大評価値の反転を比較
            is_greater = -max_child_move_eval > parent_eval;
            comparison_value = -parent_eval;
            break;
        }

        if (is_greater) {
            // 分岐その1: 条件を満たす全ての子ポジションのリンクやリーフを出力
            for (const auto& link : child_position.links) {
                if (link.eval_link > comparison_value) {
                    std::string move_str, updated_kifu;
                    std::tie(move_str, updated_kifu) = convert_move_to_str(link.move, kifu, manager);
                    output_file << updated_kifu << std::endl;
                    manager.debug_log("Mismatch found (multiple moves). Kifu: " + updated_kifu + " (Move: " + std::to_string(link.move) + ")", PositionManager::LogLevel::DEBUG);
                }
            }
            if (child_position.leaf.eval > comparison_value) {
                std::string move_str, updated_kifu;
                std::tie(move_str, updated_kifu) = convert_move_to_str(child_position.leaf.move, kifu, manager);
                output_file << updated_kifu << std::endl;
                manager.debug_log("Mismatch found (leaf move). Kifu: " + updated_kifu + " (Move: " + std::to_string(child_position.leaf.move) + ")", PositionManager::LogLevel::DEBUG);
            }
        }
        else {
            // 分岐その2: 判定に使った子ポジションの手までの棋譜を出力
            uint8_t max_child_move = 0;
            for (const auto& link : child_position.links) {
                if (link.eval_link == max_child_move_eval) {
                    max_child_move = link.move;
                    break;
                }
            }
            if (child_position.leaf.eval == max_child_move_eval) {
                max_child_move = child_position.leaf.move;
                manager.debug_log("Leaf evaluation used for max_child_move_eval", PositionManager::LogLevel::INFO);
            }

            std::string move_str, updated_kifu;
            std::tie(move_str, updated_kifu) = convert_move_to_str(max_child_move, kifu, manager);
            output_file << updated_kifu << std::endl;
            manager.debug_log("Mismatch found (single move). Kifu: " + updated_kifu + " (Move: " + std::to_string(max_child_move) + ")", PositionManager::LogLevel::DEBUG);
        }
    }
}

void main_process_recursive(Position& current_position, std::string current_kifu, const std::string& output_path, PositionManager& manager, int mode){
    // ループカウンターをインクリメント
    manager.loop_count++;

    // ループごとにコマンドラインの表示を更新（表示頻度を調整可能）
    if (manager.loop_count == 1 || manager.loop_count % 100000 == 0) {
        std::cout << "\r" << manager.loop_count << " Links or Leaf processed" << std::flush;
    }
    // パスの処理
    if (current_kifu.length() >= 4 && current_kifu.substr(current_kifu.length() - 4) == "Pass") {
        current_kifu = current_kifu.substr(0, current_kifu.length() - 4);
        manager.debug_log("Pass detected, updated kifu: " + current_kifu, PositionManager::LogLevel::DEBUG);
        manager.current_kifu = current_kifu;
    }

    // 子positionを得る
    Position child_position;
    std::string new_kifu, transformation_name;
    uint8_t move;
    while (true){
        manager.current_position = current_position;
        manager.current_kifu = current_kifu;
        manager.debug_log("Current position: " + format_position(current_position), PositionManager::LogLevel::DEBUG);
        manager.debug_log("Current kifu: " + current_kifu, PositionManager::LogLevel::DEBUG);

        std::tie(child_position, new_kifu, transformation_name, move) = get_children(manager, current_position);

        // 最終関数起動条件を満たした時、理由と共にデバッグログに出力して起動
        if (transformation_name == "child_not_found") {
            manager.debug_log("Child position not found. Ending current branch.", PositionManager::LogLevel::DEBUG);
            break;
        }
            
        // 比較関数と不一致の場合出力をする関数を呼び出し
        else {
            bool mismatch = judge_mismatch(child_position, current_position, move, mode, manager);
            if (mismatch) {
                mismatch_process(child_position, new_kifu, transformation_name, output_path, manager,
                    child_position.eval_value, current_position.eval_value, mode);
            }

            // 親ポジションを更新
            manager.current_position = child_position;
            manager.current_kifu = new_kifu;

            // 先頭へ戻る (子positionで同じ処理を行う)
            main_process_recursive(child_position, new_kifu, output_path, manager, mode);
        }
    }
}

// メイン関数　本来スタック管理と不一致の発見は関数を分けるべきなんだろうけれども　最初の部分は開始処理
void main_process(const std::string& output_path, PositionManager& manager, int mode) {
    try {
        // プログラム全体の実行時間の測定
        manager.program_start_time = std::chrono::steady_clock::now();

        // 初期局面をbookから読み取る
        const Position* initial_book_position = read_position(0x0000000810000000ULL, 0x0000001008000000ULL);
        if (!initial_book_position) {
            manager.debug_log("Initial position not found in book. Terminating program.", PositionManager::LogLevel::ERROR);
            std::exit(1);
        }

        // 初期局面を設定
        manager.current_position = *initial_book_position;
        manager.current_kifu = "";

        // メイン処理 (再帰的に実装)
        main_process_recursive(manager.current_position, manager.current_kifu, output_path, manager, mode);
    }
    // エラー処理が起動される日は来るのだろうか
    catch (const std::exception& e) {
        manager.debug_log("Exception in main_process: " + std::string(e.what()), PositionManager::LogLevel::ERROR);
        std::cerr << "Critical error in main_process: " << e.what() << std::endl;
        std::exit(1);// プログラムを終了
    }

    // 最終ループ数を出力、デバッグログにも最終ループ数を記録
    std::cout << "\r" << manager.loop_count << " Links or Leaf processed (Final)" << std::endl;
    manager.debug_log("Total Links or Leaf processed: " + std::to_string(manager.loop_count), PositionManager::LogLevel::WARNING);

    // プログラム全体の実行時間を計算して実行時間をログに出力、コンソールにも実行時間を出力
    auto program_end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> program_duration = program_end_time - manager.program_start_time;
    manager.debug_log("Total program execution time: " + std::to_string(program_duration.count()) + " seconds", PositionManager::LogLevel::WARNING);
    std::cout << "Total program execution time: " << program_duration.count() << " seconds" << std::endl;
}

std::tuple<Position, std::string, std::string, uint8_t> get_children(PositionManager& manager, Position& position) {
    try {

        // リンクの処理
        for (auto& link : position.links) {
            if (!link.visited) {
                link.visited = true;  // リンクを訪問済みにマーク
                manager.debug_log("Unvisited link found: Move=" + std::to_string(link.move) + ", Eval=" + std::to_string(link.eval_link) + ", Visited: False", PositionManager::LogLevel::DEBUG);
                auto [child_position, new_kifu, transformation] = process_position(position, manager.current_kifu, link.move, manager);
                if (transformation != "child_not_found") {
                    // 返値: 子ポジション、新しい棋譜、変換名、リンクの手の値のタプル
                    return std::make_tuple(child_position, new_kifu, transformation, link.move);
                }
            }
        }

        // リーフの処理
        if (!(position.leaf.move == 0 && position.leaf.eval == 0 && !position.leaf.visited) && position.leaf.move != 65) {
            if (!position.leaf.visited) {
                position.leaf.visited = true;  // リーフを訪問済みにマーク
                manager.debug_log("Unvisited leaf found: Move=" + std::to_string(position.leaf.move) + ", Eval=" + std::to_string(position.leaf.eval) + ", Visited: False", PositionManager::LogLevel::DEBUG);
                auto [child_position, new_kifu, transformation] = process_position(position, manager.current_kifu, position.leaf.move, manager);
                if (transformation != "child_not_found") {
                    // 返値: 子ポジション、新しい棋譜、変換名、リーフの手の値のタプル
                    return std::make_tuple(child_position, new_kifu, transformation, position.leaf.move);
                }
            }
        }
        // move値が65 noneの場合は処理をスキップしてデバッグ出力のみ
        else if (position.leaf.move == 65) {
            manager.debug_log("Leaf with move value 65 encountered. Skipping processing.", PositionManager::LogLevel::DEBUG);
        }
        // 返値: 空のポジション、現在の棋譜、"child_not_found"文字列、0のタプル（探索終了を指示）
        return std::make_tuple(Position(), manager.current_kifu, "child_not_found", static_cast<uint8_t>(0));
    }
    catch (const std::exception& e) {
        manager.debug_log("Critical error in get_children: " + std::string(e.what()), PositionManager::LogLevel::ERROR);
        std::cerr << "Critical error: " << e.what() << std::endl;
        std::exit(1);  // プログラムを終了
    }
}

std::tuple<Position, std::string, std::string> process_position(Position& position, const std::string& kifu, uint8_t move, PositionManager& manager) {

    // 子ポジションを生成し、一時変数に保存する
    Position original_child_position;
    std::string new_kifu;
    std::tie(original_child_position, new_kifu) = create_position_data(manager, move);
    manager.debug_log("Generated original child position: " + format_position(original_child_position), PositionManager::LogLevel::DEBUG);
    manager.debug_log("New kifu: " + new_kifu, PositionManager::LogLevel::DEBUG);

    // 親ポジションの正規化とフラグ更新を行う
    auto [normalized_parent, parent_transformation] = normalize_position(position.my_stones, position.opponent_stones, manager);
    auto normalized_parent_key = std::make_pair(std::get<0>(normalized_parent), std::get<1>(normalized_parent));

    // 正規化された親ポジションをread_positionを使用して取得
    const Position* normalized_parent_position = read_position(std::get<0>(normalized_parent), std::get<1>(normalized_parent));
    //　万一見つからなかった場合のエラー処理
    if (!normalized_parent_position) {
        manager.debug_log("Critical error: Parent position not found in book", PositionManager::LogLevel::ERROR);
        std::cerr << "Critical error: Parent position not found in book. Terminating program." << std::endl;
        std::exit(1);  // プログラムを終了
    }
    manager.debug_log("Book position retrieved: " + format_position(*normalized_parent_position), PositionManager::LogLevel::DEBUG);

    // 正規化された親ポジションの該当する手のVisitedフラグを直接更新
    uint8_t normalized_move = normalize_move(move, parent_transformation, manager);
    // 直接更新したいのでconstが付いているread_position関数は使えない
    auto it = book_positions.find(normalized_parent_key);
    if (it != book_positions.end()) {
        Position& book_position = it->second;
        bool updated = false;
        for (Link& link : book_position.links) {
            if (link.move == normalized_move) {
                link.visited = true;
                manager.debug_log("Parent link visited flag updated: move=" + std::to_string(normalized_move) + ", visited=True", PositionManager::LogLevel::DEBUG);
                updated = true;
                break;
            }
        }
        // リーフも同様に処理
        if (!updated && book_position.leaf.move == normalized_move) {
            book_position.leaf.visited = true;
            manager.debug_log("Parent leaf visited flag updated: move=" + std::to_string(normalized_move) + ", visited=True", PositionManager::LogLevel::DEBUG);
            updated = true;
        }
            // 更新された正規化親ポジションをbook_positionに直接保存
        if (updated) {
            manager.debug_log("Updated parent book position: " + format_position(book_position), PositionManager::LogLevel::DEBUG);
        }
    }

    // 子ポジションを正規化し、bookと照合する
    std::tuple<uint64_t, uint64_t> normalized_child_position;
    std::string transformation;
    std::tie(normalized_child_position, transformation) = normalize_position(original_child_position.my_stones, original_child_position.opponent_stones, manager);

    uint64_t normalized_child_my_stones = std::get<0>(normalized_child_position);
    uint64_t normalized_child_opponent_stones = std::get<1>(normalized_child_position);

    const Position* book_child_position = read_position(normalized_child_my_stones, normalized_child_opponent_stones);

    if (book_child_position) {
        manager.debug_log("Child position found in book: " + format_position(*book_child_position), PositionManager::LogLevel::DEBUG);

        // bookから得られた情報を使って、正規化前の子ポジションを更新する
        original_child_position.links = book_child_position->links;
        original_child_position.leaf = book_child_position->leaf;
        original_child_position.eval_value = book_child_position->eval_value;

        // move値を正規化前の状態に戻す
        for (auto& link : original_child_position.links) {
            link.move = denormalize_move(link.move, transformation, manager);
        }
        original_child_position.leaf.move = denormalize_move(original_child_position.leaf.move, transformation, manager);

        manager.debug_log("Final denormalized child position: " + format_position(original_child_position), PositionManager::LogLevel::DEBUG);

        // 返値: 正規化前の子ポジション、新しい棋譜、変換名のタプル
        return std::make_tuple(original_child_position, new_kifu, transformation);
    }
    else {
        std::stringstream ss2;
        ss2 << "Child position not found in book: (my_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << normalized_child_my_stones
            << ", opponent_stones: 0x" << std::hex << std::setw(16) << std::setfill('0') << normalized_child_opponent_stones << ")";
        manager.debug_log(ss2.str(), PositionManager::LogLevel::DEBUG);

        // 返値: 空のポジション、新しい棋譜、"child_not_found"文字列のタプル（子ポジションがbookに見つからなかったことを示す）
        return std::make_tuple(Position(), new_kifu, "child_not_found");
    }
}
// moveの例外処理と関数二つの呼び出し
std::tuple<Position, std::string> create_position_data(PositionManager& manager, int move) {
    std::string move_str, new_kifu;

    if (move == 64) {  // パス
        move_str = "Pass";
        new_kifu = manager.current_kifu + move_str;
        manager.debug_log("Pass move detected. New kifu: " + new_kifu, PositionManager::LogLevel::DEBUG);

        // パスの場合はflip_stonesと同様の処理を行うが、石の反転は行わない
        Position child_position;
        child_position.my_stones = manager.current_position.opponent_stones;
        child_position.opponent_stones = manager.current_position.my_stones;
        child_position.eval_value = -manager.current_position.eval_value;  // 評価値を反転

        // 返値: 新しいポジション、更新された棋譜のタプル
        return std::make_tuple(child_position, new_kifu);
    }
    // ここに65が来ることはあり得ないはず
    else if (move == 65) {  // 無効な手
        move_str = "None";
        new_kifu = manager.current_kifu + move_str;
        manager.debug_log("Invalid move (None) detected. Terminating program. New kifu: " + new_kifu, PositionManager::LogLevel::ERROR);
        // プログラムを終了
        std::exit(1);
    }
    // 二つの関数を呼び出して子ポジションを実際に作るところ
    std::tie(move_str, new_kifu) = convert_move_to_str(move, manager.current_kifu, manager);

    Position child_position = flip_stones(manager.current_position, move_str, manager);
    child_position.links.clear();

    // 返値: 手を表す文字列、更新された棋譜のタプル
    return std::make_tuple(child_position, new_kifu);
}
// moveを棋譜に変換する
std::tuple<std::string, std::string> convert_move_to_str(int move, const std::string& kifu, PositionManager& manager) {
    char col = 'a' + (move % 8);
    int row = (move / 8) + 1;
    std::string move_str = std::string(1, col) + std::to_string(row);

    // 棋譜を更新
    std::string previous_kifu = kifu.empty() ? manager.current_kifu : kifu;
    std::string new_kifu = previous_kifu + move_str;
    manager.debug_log("Updated kifu: " + new_kifu, PositionManager::LogLevel::DEBUG);

    manager.current_kifu = new_kifu;
    return std::make_tuple(move_str, new_kifu);
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
//　ひっくり返し関数本体
Position flip_stones(const Position& position, const std::string& move_str, PositionManager& manager) {
    uint64_t my_stones = position.my_stones;
    uint64_t opponent_stones = position.opponent_stones;

    // 打つ手の変換
    int move_index = ((8 * (8 - (move_str[1] - '0'))) + (7 - static_cast<int>(move_str[0]) + 'a'));
    uint64_t move = 1ULL << move_index;

    // 石のひっくり返し
    uint64_t flipped = flip_all_directions(my_stones, opponent_stones, move);
    my_stones |= move | flipped;
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

//　正規化とはこれのこと　これのせいで散々苦労したその1
std::tuple<std::tuple<uint64_t, uint64_t>, std::string> normalize_position(uint64_t my_stones, uint64_t opponent_stones, PositionManager& manager) {
    std::tuple<uint64_t, uint64_t> min_value = std::make_tuple(my_stones, opponent_stones);
    std::string min_transformation = "identity";

    //　変換用辞書
    const std::unordered_map<std::string, std::function<uint64_t(uint64_t)>> transformations = {
        {"rotate_90", rotate_90},
        {"rotate_180", rotate_180},
        {"rotate_270", rotate_270},
        {"flip_vertical", flip_vertical},
        {"flip_horizontal", flip_horizontal},
        {"flip_diag_a1h8", flip_diag_a1h8},
        {"flip_diag_a8h1", flip_diag_a8h1}
    };

    //　最小の64bit値を出す変換がどれか調べて最小値への変換を採用
    for (const auto& [name, transform] : transformations) {
        uint64_t transformed_my_stones = transform(my_stones);
        uint64_t transformed_opponent_stones = transform(opponent_stones);
        if (std::make_tuple(transformed_my_stones, transformed_opponent_stones) < min_value) {
            min_value = std::make_tuple(transformed_my_stones, transformed_opponent_stones);
            min_transformation = name;
        }
    }

    //　デバッグログを出力するのに整形処理がいる
    std::stringstream ss;
    ss << "Final min transformation: " << min_transformation
        << ", min_value: (my_stones=0x" << std::hex << std::setw(16) << std::setfill('0') << std::get<0>(min_value)
        << ", opponent_stones=0x" << std::hex << std::setw(16) << std::setfill('0') << std::get<1>(min_value) << ")";
    manager.debug_log(ss.str(), PositionManager::LogLevel::DEBUG);

    // 返値: 正規化された盤面（自分の石、相手の石のタプル）と変換名のタプル
    return std::make_tuple(min_value, min_transformation);
}

//　move値の正規化処理　結局必要になってしまった ここにpassがいくことはある　noneはないはずなのでエラー出力して落とそう
int normalize_move(int move, const std::string& transformation_name, PositionManager& manager) {
    manager.debug_log("Normalizing move: " + std::to_string(move) + ", using transformation: " + transformation_name, PositionManager::LogLevel::DEBUG);

    if (move == 64) {  // パス
        manager.debug_log("At normalize move is a pass, returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }
    else if (move == 65) {  // 無効な手
        manager.debug_log("Invalid move (None) detected. Terminating program.", PositionManager::LogLevel::ERROR);
        std::exit(1);  // プログラムを終了
    }
    //　かたくなにunorderd mapしか使わない
    const std::unordered_map<std::string, std::function<int(int)>> transformations = {
        {"identity", [](int x) { return x; }},
        {"rotate_90", rotate_move_90},
        {"rotate_180", rotate_move_180},
        {"rotate_270", rotate_move_270},
        {"flip_vertical", flip_move_vertical},
        {"flip_horizontal", flip_move_horizontal},
        {"flip_diag_a1h8", flip_move_diag_a1h8},
        {"flip_diag_a8h1", flip_move_diag_a8h1}
    };
    //　move値の正規化処理　ここも前で何を使った記憶しておけば十分
    const auto& transform = transformations.at(transformation_name);

    // 返値: 正規化された手の値
    return transform(move);
}

//　非正規化があるのはmove値のみ ここにnoneが行くことはあるので落としてはいけない。全消し時とか
int denormalize_move(int move, const std::string& transformation_name, PositionManager& manager) {
    manager.debug_log("Denormalizing move: " + std::to_string(move) + ", using transformation: " + transformation_name, PositionManager::LogLevel::DEBUG);

    if (move == 64) {
        manager.debug_log("At denormalize move is a pass, returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }
    else if (move == 65) {
        manager.debug_log("Move is invalid (none), returning move unchanged.", PositionManager::LogLevel::DEBUG);
        return move;
    }

    //　逆変換用辞書
    const std::unordered_map<std::string, std::function<int(int)>> reverse_transformations = {
        {"identity", [](int x) { return x; }},
        {"rotate_90", rotate_move_270},
        {"rotate_180", rotate_move_180},
        {"rotate_270", rotate_move_90},
        {"flip_vertical", flip_move_vertical},
        {"flip_horizontal", flip_move_horizontal},
        {"flip_diag_a1h8", flip_move_diag_a1h8},
        {"flip_diag_a8h1", flip_move_diag_a8h1}
    };

    //　前回使った正規化関数を覚えておいて、その逆変換関数を使用
    const auto& reverse_transform = reverse_transformations.at(transformation_name);
    return reverse_transform(move);
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
const Position* read_position(uint64_t my_stones, uint64_t opponent_stones) {
    auto it = book_positions.find(std::make_pair(my_stones, opponent_stones));
    return (it != book_positions.end()) ? &(it->second) : nullptr;
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
        const Position* position = read_position(my_position, opponent_position);
        if (position) {
            std::stringstream ss;
            ss << "Position found - My stones: " << my_position_str
                << ", Opponent stones: " << opponent_position_str
                << "\n" << format_position(*position);
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

// メイン関数　ファイルパスの指定とコンフィグ読み込み→book読み込み→メイン関数読み込み
int main() {
    std::string book_path = "book.dat";
    std::string debug_log_path = "debuglog.txt";
    std::string output_path = "mismatched_positions.txt";
    std::string config_path = "config.ini";
    std::string specified_positions_path = "specified_positions.txt";

    try {
        auto [log_level, auto_adjust, adjusted_level, mode] = read_config(config_path);
        PositionManager manager(book_path, debug_log_path, log_level, auto_adjust, adjusted_level);

        if (mode < 1 || mode > 5) {
            std::cerr << "Error: Invalid mode (" << mode << "). Mode must be between 1 and 5." << std::endl;
            manager.debug_log("Invalid mode: " + std::to_string(mode), PositionManager::LogLevel::ERROR);
            return 1;  // エラーコードを返して終了
        }

        load_all_positions(book_path, manager);

        switch (mode) {
        case 1:
        case 2:
        case 3:
        case 4:
            main_process(output_path, manager, mode);
            break;
        case 5:
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
    // 返値　虚無
    return 0;
}
