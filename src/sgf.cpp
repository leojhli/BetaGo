#include "betago/sgf.hpp"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace betago {
namespace {
constexpr std::size_t max_bytes = 32U * 1024U * 1024U;
constexpr std::size_t max_nodes = 200000;
constexpr int max_depth = 256;
using Node = std::map<std::string, std::vector<std::string>>;

bool whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\v' || value == '\f';
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\v\f");
    return value.substr(first, last - first + 1);
}
std::string simple_text(std::string value) {
    for (auto& character : value) if (whitespace(character)) character = ' ';
    return value;
}
std::string latin1_utf8(std::string_view value) {
    std::string result;
    for (unsigned char byte : value) {
        if (byte < 128) result.push_back(static_cast<char>(byte));
        else {
            result.push_back(static_cast<char>(0xc0U | (byte >> 6U)));
            result.push_back(static_cast<char>(0x80U | (byte & 0x3fU)));
        }
    }
    return result;
}
std::string fingerprint(std::string_view bytes) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) { hash ^= byte; hash *= UINT64_C(1099511628211); }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

// Parse the full syntax, while retaining only the selected first variation.
// Ignored branches still count against the depth and node limits.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    std::vector<std::vector<Node>> collection() {
        std::vector<std::vector<Node>> games;
        // A UTF-8 byte order mark is accepted at the start of a file.
        if (text_.starts_with("\xef\xbb\xbf")) cursor_ = 3;
        skip();
        while (cursor_ < text_.size()) {
            if (text_[cursor_] != '(') fail("Expected a game tree");
            games.push_back(tree(1, true));
            skip();
        }
        if (games.empty()) fail("The collection is empty");
        return games;
    }
    std::size_t nodes() const { return nodes_; }
    std::size_t variations() const { return variations_; }
private:
    std::string_view text_;
    std::size_t cursor_ = 0, nodes_ = 0, variations_ = 0;
    [[noreturn]] void fail(const char* message) const {
        throw std::invalid_argument(std::string("SGF: ") + message + " at byte " + std::to_string(cursor_));
    }
    void skip() { while (cursor_ < text_.size() && whitespace(text_[cursor_])) ++cursor_; }
    void expect(char token) {
        skip();
        if (cursor_ == text_.size() || text_[cursor_] != token) fail("Unexpected syntax");
        ++cursor_;
    }
    std::string value() {
        expect('[');
        std::string result;
        while (cursor_ < text_.size()) {
            char character = text_[cursor_++];
            if (character == ']') return result;
            if (character == '\0') fail("NUL characters are unsupported");
            if (character == '\\') {
                if (cursor_ == text_.size()) fail("Unfinished value escape");
                character = text_[cursor_++];
                if (character == '\r' || character == '\n') {
                    if (cursor_ < text_.size() &&
                        ((character == '\r' && text_[cursor_] == '\n') ||
                         (character == '\n' && text_[cursor_] == '\r'))) ++cursor_;
                    continue; // SGF soft line break.
                }
                if (character == '\0') fail("NUL characters are unsupported");
            } else if (character == '\r' || character == '\n') {
                if (cursor_ < text_.size() &&
                    ((character == '\r' && text_[cursor_] == '\n') ||
                     (character == '\n' && text_[cursor_] == '\r'))) ++cursor_;
                character = '\n';
            }
            result.push_back(character);
        }
        fail("Unclosed property value");
    }
    Node node() {
        expect(';');
        if (++nodes_ > max_nodes) fail("Node limit exceeded");
        Node result;
        skip();
        while (cursor_ < text_.size() && text_[cursor_] >= 'A' && text_[cursor_] <= 'Z') {
            const auto start = cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= 'A' && text_[cursor_] <= 'Z') ++cursor_;
            std::string key(text_.substr(start, cursor_ - start));
            if (result.contains(key)) fail("Duplicate property in a node");
            std::vector<std::string> values;
            skip();
            while (cursor_ < text_.size() && text_[cursor_] == '[') {
                values.push_back(value());
                skip();
            }
            if (values.empty()) fail("A property needs a bracketed value");
            result.emplace(std::move(key), std::move(values));
        }
        return result;
    }
    std::vector<Node> tree(int depth, bool keep) {
        if (depth > max_depth) fail("Variation depth limit exceeded");
        expect('(');
        skip();
        if (cursor_ == text_.size() || text_[cursor_] != ';') fail("A tree needs a node sequence");
        std::vector<Node> selected;
        while (cursor_ < text_.size() && text_[cursor_] == ';') {
            auto next = node();
            if (keep) selected.push_back(std::move(next));
            skip();
        }
        bool first = true;
        while (cursor_ < text_.size() && text_[cursor_] == '(') {
            if (!first) ++variations_;
            auto child = tree(depth + 1, keep && first);
            if (keep && first)
                for (auto& next : child) selected.push_back(std::move(next));
            first = false;
            skip();
        }
        expect(')');
        return selected;
    }
};

std::string one(const Node& node, const char* key) {
    const auto found = node.find(key);
    if (found == node.end()) throw std::invalid_argument(std::string("SGF requires explicit ") + key);
    if (found->second.size() != 1)
        throw std::invalid_argument(std::string("SGF ") + key + " needs exactly one value");
    return trim(simple_text(found->second.front()));
}
int size_value(const std::string& value) {
    int size = 0;
    const auto start = value.data() + (!value.empty() && value.front() == '+' ? 1 : 0);
    auto [end, error] = std::from_chars(start, value.data() + value.size(), size);
    if (error != std::errc{} || end != value.data() + value.size() || size < 1 || size > 19)
        throw std::invalid_argument("SGF size must be a square board from 1 to 19");
    return size;
}
double real_value(const std::string& value, const char* name) {
    // SGF Real is decimal notation, without exponents, NaN, or infinity.
    std::size_t index = !value.empty() && (value[0] == '+' || value[0] == '-') ? 1 : 0;
    const auto digits_start = index;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9') ++index;
    if (index == digits_start)
        throw std::invalid_argument(std::string("SGF ") + name + " must be a finite decimal");
    if (index < value.size() && value[index] == '.') {
        const auto fraction_start = ++index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') ++index;
        if (index == fraction_start)
            throw std::invalid_argument(std::string("SGF ") + name + " must be a finite decimal");
    }
    if (index != value.size())
        throw std::invalid_argument(std::string("SGF ") + name + " must be a finite decimal");
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    const bool nonzero_decimal = value.find_first_of("123456789") != std::string::npos;
    if (end != value.data() + value.size() || !std::isfinite(number) || (number == 0 && nonzero_decimal))
        throw std::invalid_argument(std::string("SGF ") + name + " must be a finite decimal");
    return number;
}
int result_value(std::string value) {
    for (auto& character : value)
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    if (value == "0" || value == "DRAW") return EMPTY;
    if (value.size() < 2 || (value[0] != 'B' && value[0] != 'W') || value[1] != '+')
        throw std::invalid_argument("SGF requires a known win or draw result");
    const auto reason = value.substr(2);
    if (!reason.empty() && reason != "R" && reason != "RESIGN" && real_value(reason, "result margin") <= 0)
        throw std::invalid_argument("SGF winning margin must be positive");
    return value[0] == 'B' ? BLACK : WHITE;
}
Move move_value(const std::string& value, int size) {
    if (value.empty() || (value == "tt" && size <= 19)) return PASS;
    if (value.size() != 2 || value[0] < 'a' || value[1] < 'a' ||
        value[0] >= 'a' + size || value[1] >= 'a' + size)
        throw std::invalid_argument("SGF move is outside the requested board");
    return Point{value[1] - 'a', value[0] - 'a'};
}
void validate_node(const Node& node, bool root) {
    for (const auto* unsupported : {"AB", "AW", "AE", "HA", "PL", "KO"})
        if (node.contains(unsupported))
            throw std::invalid_argument(std::string("SGF setup, handicap, turn and legality overrides are unsupported: ") + unsupported);
    if (!root)
        for (const auto* critical : {"FF", "GM", "SZ", "KM", "RE", "RU", "PB", "PW", "CA", "AP", "ST"})
            if (node.contains(critical))
                throw std::invalid_argument(std::string("SGF critical game information must be in the root: ") + critical);
    if (node.contains("B") && node.contains("W"))
        throw std::invalid_argument("SGF node cannot contain both Black and White moves");
}

Json state_json(const GameState& state) {
    return {{"board", state.board()}, {"to_play", state.to_play()}, {"komi", state.komi()},
            {"consecutive_passes", state.consecutive_passes()},
            {"previous_board", state.previous_board() ? Json(*state.previous_board()) : Json(nullptr)}};
}
Json moves_json(const std::vector<Move>& moves) {
    Json result = Json::array();
    for (Move move : moves) result.push_back(move ? Json::array({move->row, move->column}) : Json(nullptr));
    return result;
}
GameState validate_game(const ExpertGame& game) {
    if (game.initial_state.size() < 1 || game.initial_state.size() > 19)
        throw std::invalid_argument("Expert board size must be between 1 and 19");
    if (game.winner != EMPTY && game.winner != BLACK && game.winner != WHITE)
        throw std::invalid_argument("Expert winner must be EMPTY, BLACK, or WHITE");
    if (!game.metadata.is_object()) throw std::invalid_argument("Expert metadata must be an object");
    try {
        // Provenance will be saved as JSON. Reject invalid UTF-8 now, rather
        // than leaving an imported game that fails only when the file is saved.
        static_cast<void>(game.metadata.dump());
        static_cast<void>(Json(game.source).dump());
    } catch (const Json::exception&) {
        throw std::invalid_argument("Expert source and metadata must be valid UTF-8 JSON text");
    }
    if (game.moves.empty() || game.moves.size() > max_nodes)
        throw std::invalid_argument("Expert games need between 1 and 200000 moves");
    auto state = game.initial_state;
    for (Move move : game.moves) state = state.play(move);
    return state;
}
const Json& field(const Json& record, const char* key) {
    if (!record.is_object() || !record.contains(key))
        throw std::invalid_argument(std::string("Expert record is missing ") + key);
    return record.at(key);
}
int integer(const Json& value, int minimum, int maximum, const char* name) {
    if (!value.is_number_integer()) throw std::invalid_argument(std::string(name) + " must be an integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum) || number < static_cast<std::uint64_t>(minimum))
            throw std::invalid_argument(std::string(name) + " is out of range");
        return static_cast<int>(number);
    }
    const auto number = value.get<std::int64_t>();
    if (number < minimum || number > maximum) throw std::invalid_argument(std::string(name) + " is out of range");
    return static_cast<int>(number);
}
Board board_json(const Json& value) {
    if (!value.is_array() || value.empty() || value.size() > 19)
        throw std::invalid_argument("Expert board must be square with size 1 to 19");
    Board board;
    for (const auto& row : value) {
        if (!row.is_array() || row.size() != value.size()) throw std::invalid_argument("Expert board must be square");
        std::vector<int> cells;
        for (const auto& cell : row) cells.push_back(integer(cell, EMPTY, WHITE, "Expert stone"));
        board.push_back(std::move(cells));
    }
    return board;
}
GameState state_from_json(const Json& value) {
    auto board = board_json(field(value, "board"));
    int player = integer(field(value, "to_play"), BLACK, WHITE, "Expert player");
    int passes = integer(field(value, "consecutive_passes"), 0, 2, "Expert pass count");
    const auto& komi = field(value, "komi");
    if (!komi.is_number() || !std::isfinite(komi.get<double>())) throw std::invalid_argument("Expert komi must be finite");
    std::optional<Board> previous;
    if (!field(value, "previous_board").is_null()) previous = board_json(field(value, "previous_board"));
    return GameState(std::move(board), player, komi.get<double>(), passes, std::move(previous));
}
std::string escaped(std::string_view value) {
    std::string result;
    for (char character : value) {
        if (character == '\\' || character == ']') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}
std::string decimal(double value) {
    // SGF Real disallows exponents. Fixed notation also survives strict import.
    char buffer[1024];
    auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    if (error != std::errc{}) throw std::invalid_argument("Cannot export SGF decimal");
    return std::string(buffer, end);
}
} // namespace

std::vector<ExpertGame> parse_sgf(std::string_view text, int required_size, std::string source) {
    if (text.size() > max_bytes) throw std::invalid_argument("SGF input exceeds 32 MiB");
    if (required_size < 1 || required_size > 19) throw std::invalid_argument("Required SGF size must be from 1 to 19");
    Parser parser(text);
    auto trees = parser.collection();
    const auto source_fingerprint = fingerprint(text);
    std::vector<ExpertGame> games;
    for (std::size_t index = 0; index < trees.size(); ++index) {
        const auto& nodes = trees[index];
        const auto& root = nodes.front();
        validate_node(root, true);
        if (root.contains("FF") && size_value(one(root, "FF")) != 4) throw std::invalid_argument("Only SGF FF[4] is supported");
        if (root.contains("GM") && size_value(one(root, "GM")) != 1) throw std::invalid_argument("Only SGF Go GM[1] is supported");
        std::string charset = root.contains("CA") ? one(root, "CA") : "ISO-8859-1";
        for (auto& character : charset)
            if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
        const bool latin1 = charset == "ISO-8859-1" || charset == "ISO8859-1" || charset == "LATIN1";
        if (!latin1 && charset != "UTF-8" && charset != "UTF8")
            throw std::invalid_argument("SGF text encoding must be UTF-8 or ISO-8859-1");
        const int size = size_value(one(root, "SZ"));
        if (size != required_size) throw std::invalid_argument("SGF board size does not match the requested size; boards are never cropped");
        ExpertGame game;
        game.initial_state = GameState::new_game(size, real_value(one(root, "KM"), "komi"));
        game.winner = result_value(one(root, "RE"));
        game.source = source;
        game.metadata = {{"format", "SGF FF4 Go"}, {"mainline_only", true},
            {"rules_mismatch_possible", true}, {"value_label", "reported SGF result"},
            {"CA", charset}, {"ignored_markup_and_comments", true},
            {"collection_index", index}, {"source_bytes", text.size()},
            {"source_fingerprint_fnv1a64", source_fingerprint}, {"parsed_nodes", parser.nodes()},
            {"ignored_variations", parser.variations()}};
        for (const auto* key : {"RU", "PB", "PW", "RE"})
            if (root.contains(key)) {
                const auto value = one(root, key);
                game.metadata[key] = latin1 ? latin1_utf8(value) : value;
            }
        auto state = game.initial_state;
        for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            const auto& node = nodes[node_index];
            validate_node(node, node_index == 0);
            const bool black = node.contains("B"), white = node.contains("W");
            if (!black && !white) continue;
            const int color = black ? BLACK : WHITE;
            if (state.to_play() != color) throw std::invalid_argument("SGF moves must alternate from Black");
            Move move = move_value(one(node, black ? "B" : "W"), size);
            state = state.play(move);
            game.moves.push_back(move);
        }
        validate_game(game);
        games.push_back(std::move(game));
    }
    return games;
}

std::string expert_game_id(const ExpertGame& game) {
    validate_game(game);
    Json initial = state_json(game.initial_state);
    if (game.initial_state.komi() == 0) initial["komi"] = 0.0;
    const Json canonical{{"initial_state", std::move(initial)}, {"moves", moves_json(game.moves)}, {"winner", game.winner}};
    return fingerprint(canonical.dump());
}
Json expert_game_json(const ExpertGame& game) {
    return {{"schema_version", 1}, {"kind", "expert_game"}, {"id", expert_game_id(game)},
            {"initial_state", state_json(game.initial_state)}, {"moves", moves_json(game.moves)},
            {"winner", game.winner}, {"source", game.source}, {"metadata", game.metadata}};
}
ExpertGame expert_game_from_json(const Json& record) {
    try {
        integer(field(record, "schema_version"), 1, 1, "Expert schema version");
        if (field(record, "kind") != "expert_game") throw std::invalid_argument("Unsupported expert record kind");
        ExpertGame game;
        game.initial_state = state_from_json(field(record, "initial_state"));
        const auto& moves = field(record, "moves");
        if (!moves.is_array() || moves.empty() || moves.size() > max_nodes)
            throw std::invalid_argument("Expert record needs between 1 and 200000 moves");
        game.moves = moves_from_json(moves);
        game.winner = integer(field(record, "winner"), EMPTY, WHITE, "Expert winner");
        if (!field(record, "source").is_string()) throw std::invalid_argument("Expert source must be text");
        game.source = field(record, "source").get<std::string>();
        game.metadata = field(record, "metadata");
        if (!field(record, "id").is_string() || field(record, "id") != expert_game_id(game))
            throw std::invalid_argument("Expert content identity disagrees with the record");
        return game;
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Invalid expert record: ") + error.what());
    }
}
std::string expert_game_sgf(const ExpertGame& game) {
    const auto final_state = validate_game(game);
    if (game.initial_state != GameState::new_game(game.initial_state.size(), game.initial_state.komi()))
        throw std::invalid_argument("SGF export requires an empty initial board with Black to play");
    std::string result = game.winner == EMPTY ? "0" : game.winner == BLACK ? "B+" : "W+";
    if (game.winner != EMPTY && final_state.is_terminal() && final_state.winner() == game.winner) {
        const auto score = final_state.score();
        result += decimal(std::abs(score.black - score.white));
    }
    std::ostringstream out;
    out << "(;FF[4]GM[1]CA[UTF-8]AP[BetaGo:expert-export]SZ[" << game.initial_state.size()
        << "]KM[" << decimal(game.initial_state.komi()) << "]RE[" << result << ']';
    for (const auto* key : {"RU", "PB", "PW"})
        if (game.metadata.contains(key) && game.metadata.at(key).is_string())
            out << key << '[' << escaped(game.metadata.at(key).get<std::string>()) << ']';
    auto state = game.initial_state;
    for (Move move : game.moves) {
        out << ';' << (state.to_play() == BLACK ? 'B' : 'W') << '[';
        if (move) out << static_cast<char>('a' + move->column) << static_cast<char>('a' + move->row);
        out << ']';
        state = state.play(move);
    }
    out << ")\n";
    return out.str();
}
} // namespace betago
