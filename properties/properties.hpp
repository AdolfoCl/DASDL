#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "token_types_generated.hpp"
#include "dasdl_model.hpp"
#include "dasdl_sql.hpp"

// ---------------------------------------------------------------------------
// DASDL lexical model (8600 0213-424, Section 2 "Introduction to the Language
// Components of DASDL"; the Markdown conversion is in
// ../manuals/sections/02-introduction-to-the-language-components-of-dasdl.md)
//
//   - Free format. Nothing is column-bound: a declaration may start anywhere
//     and run across as many lines as it likes.
//   - Identifiers are <letter> then letters, digits and hyphens, ending on a
//     letter or digit (EMPLOYEE, AS-OF-DATE, B-1). The hyphen is part of the
//     word, exactly as in COBOL-74 and unlike WFL — which is also why the
//     grammar can spell DATA-SET, HIGH-VALUE and LOW-VALUE as single words.
//   - A remark starts at '%' and runs to the end of the line. It may appear
//     anywhere except inside a string, so it is consumed here and never
//     reaches the grammar.
//   - Strings are "double quoted"; two adjacent quotation marks stand for one
//     quotation mark. A string is also the stored form of a comment, which is
//     why the grammar accepts an optional STRTOK after a structure name.
//   - The COMMENT ... ; form of a comment (Section 2, "Comment") is NOT
//     handled yet: its terminating semicolon is the same character that ends a
//     declaration, so swallowing it in the lexer would eat the terminator the
//     grammar is waiting for. Use the '%' or "..." forms.
//   - An integer is a digit run; a sign binds to it only when written with no
//     intervening space (+511, -123456). A number carries a decimal point.
//   - Keyword status is not absolute in DASDL — the manual reserves no word
//     list, and structures can be named freely. So a word is stamped RESTOK
//     only when the grammar itself mentions it (Trackway emits that list into
//     the generated workfile) and IDNTOK otherwise; Token::operator== then
//     lets the two match by spelling, so a grammar word still matches where
//     the source used it as a plain name.
// ---------------------------------------------------------------------------

/** Characters allowed after the first letter of a DASDL identifier. */
inline bool dasdl_identifier_body_char(unsigned char c) {
    return std::isalnum(c) || c == '-';
}

/** Single characters that are tokens in their own right. */
inline bool dasdl_special_char(unsigned char c) {
    return c == '(' || c == ')' || c == ';' || c == ',' || c == '=' ||
           c == '.' || c == ':' || c == '/' || c == '*' || c == '+' ||
           c == '-' || c == '<' || c == '>' || c == '&' || c == '^';
}

class Token {
public:
    unsigned char type;
    /** Lexeme text: word spelling, digits, or the string body without quotes. */
    std::string value;
    int line = 0;
    int column = 0;
    /** Parsed magnitude for numeric literal tokens (INTTOK / NUMTOK). */
    long double numericValue = 0.0L;
    /** Rounded integer form of ``numericValue`` (INTTOK only). */
    int intValue = 0;

    Token(unsigned char type_in, std::string value_in, int line_in = 0, int column_in = 0)
        : type(type_in), value(std::move(value_in)), line(line_in), column(column_in) {
        refresh_numeric_fields();
    }

    void refresh_numeric_fields() {
        numericValue = 0.0L;
        intValue = 0;
        if (value.empty()) return;
        if (type == INTTOK) {
            try {
                const long long ll = std::stoll(value);
                numericValue = static_cast<long double>(ll);
                intValue = static_cast<int>(ll);
            } catch (...) {
                numericValue = 0.0L;
                intValue = 0;
            }
        } else if (type == NUMTOK) {
            try {
                numericValue = std::stold(value);
            } catch (...) {
                numericValue = 0.0L;
            }
        }
    }

    static std::string upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    /** DASDL reserves no words outright, so a word the grammar spells and a
     *  word the source used as a name are the same token to us when they are
     *  spelled the same. */
    bool operator==(const Token& other) const {
        if (upper(value) != upper(other.value)) return false;
        if (type == other.type) return true;
        return (type == RESTOK && other.type == IDNTOK) ||
               (type == IDNTOK && other.type == RESTOK);
    }
};

class Parser {
public:
    explicit Parser(const std::string& filename) : file_(filename) {}

    Token nextToken() {
        Token t = nextTokenImpl();
        if (std::getenv("DASDL_TRACE_TOKENS"))
            std::cerr << "[tok] line " << t.line << "  type " << static_cast<int>(t.type)
                      << "  '" << t.value << "'\n";
        return t;
    }

    Token nextTokenImpl() {
        while (true) {
            if (line_pos_ >= line_.size()) {
                if (!read_next_line())
                    return Token(ETXTOK, "", current_line_no_, 1);
                continue;
            }

            const unsigned char c = static_cast<unsigned char>(line_[line_pos_]);

            if (std::isspace(c)) {
                ++line_pos_;
                continue;
            }

            // Remark: '%' to end of line.
            if (c == '%') {
                line_pos_ = line_.size();
                continue;
            }

            const int col = static_cast<int>(line_pos_ + 1);

            if (c == '"')
                return scan_string(col);

            if (std::isalpha(c))
                return scan_word(col);

            if (std::isdigit(c))
                return scan_number(col);

            // A sign binds to the digits only with no space between them;
            // otherwise it is an operator and stands alone.
            if ((c == '+' || c == '-') && line_pos_ + 1 < line_.size() &&
                std::isdigit(static_cast<unsigned char>(line_[line_pos_ + 1]))) {
                return scan_number(col);
            }

            if (dasdl_special_char(c)) {
                ++line_pos_;
                return Token(CHRTOK, std::string(1, static_cast<char>(c)),
                             current_line_no_, col);
            }

            ++line_pos_;
            return Token(BADTOK, std::string(1, static_cast<char>(c)),
                         current_line_no_, col);
        }
    }

    /** Word list the grammar mentions; defined in the generated workfile. */
    static std::vector<std::string> reservedWords;

    const std::string& current_line() const { return line_; }
    int current_line_number() const { return current_line_no_; }

private:
    std::ifstream file_;
    std::string line_;
    size_t line_pos_ = 0;
    int current_line_no_ = 0;

    /** Read one card image.
     *
     *  Source pulled off an MCP machine arrives as 80-column card images: the
     *  text lives in columns 1-72 and columns 73-80 carry a sequence number
     *  that is not part of the language — left in, `OPTIONS` on one card is
     *  followed by the digits of its own sequence number instead of the '('
     *  on the next. The field is only dropped when it actually looks like one,
     *  so a free-format file with content past column 72 keeps it.
     *
     *  Records with '$' in column 1 are compiler control records (SET LIST,
     *  SET SEQ, ...). They steer the MCP compiler, not the description, so
     *  they are kept for reference and never tokenized. */
    bool read_next_line() {
        while (std::getline(file_, line_)) {
            ++current_line_no_;

            if (!line_.empty() && line_.back() == '\r')
                line_.pop_back();

            if (line_.size() > kCardTextColumns &&
                is_sequence_field(line_.substr(kCardTextColumns)))
                line_.resize(kCardTextColumns);

            if (!line_.empty() && line_[0] == '$') {
                controlRecords.push_back(line_);
                continue;
            }

            line_pos_ = 0;
            return true;
        }
        return false;
    }

    static constexpr size_t kCardTextColumns = 72;

    /** Columns 73-80 are a sequence field when they hold nothing but digits
     *  and blanks, and at least one digit. */
    static bool is_sequence_field(const std::string& tail) {
        bool digit_seen = false;
        for (char ch : tail) {
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                digit_seen = true;
                continue;
            }
            if (ch != ' ') return false;
        }
        return digit_seen;
    }

public:
    /** Compiler control records ('$' in column 1) seen so far. */
    std::vector<std::string> controlRecords;

private:

    static bool is_reserved(const std::string& word) {
        const std::string up = Token::upper(word);
        return std::find(reservedWords.begin(), reservedWords.end(), up) !=
               reservedWords.end();
    }

    /** <letter> then letters, digits and hyphens. A trailing hyphen is not
     *  part of the identifier (the manual ends the word on a letter or digit),
     *  so it is handed back for the next call. */
    Token scan_word(int col) {
        const size_t start = line_pos_;
        while (line_pos_ < line_.size() &&
               dasdl_identifier_body_char(static_cast<unsigned char>(line_[line_pos_])))
            ++line_pos_;
        while (line_pos_ > start && line_[line_pos_ - 1] == '-')
            --line_pos_;
        std::string word = line_.substr(start, line_pos_ - start);
        // Classify before constructing: passing is_reserved(word) and
        // std::move(word) as arguments of one call lets the compiler move the
        // string out from under the lookup, which is unspecified order and
        // silently made every word an identifier.
        const unsigned char type = is_reserved(word) ? RESTOK : IDNTOK;
        return Token(type, std::move(word), current_line_no_, col);
    }

    /** Digits, optionally signed and optionally with one decimal point. */
    Token scan_number(int col) {
        const size_t start = line_pos_;
        if (line_[line_pos_] == '+' || line_[line_pos_] == '-')
            ++line_pos_;
        bool fractional = false;
        while (line_pos_ < line_.size()) {
            const unsigned char c = static_cast<unsigned char>(line_[line_pos_]);
            if (std::isdigit(c)) {
                ++line_pos_;
                continue;
            }
            // A point is part of the number only when a digit follows it;
            // otherwise it is the period that ends a clause.
            if (c == '.' && !fractional && line_pos_ + 1 < line_.size() &&
                std::isdigit(static_cast<unsigned char>(line_[line_pos_ + 1]))) {
                fractional = true;
                ++line_pos_;
                continue;
            }
            break;
        }
        std::string digits = line_.substr(start, line_pos_ - start);
        return Token(fractional ? NUMTOK : INTTOK, std::move(digits),
                     current_line_no_, col);
    }

    /** A string, plus any string written next to it.
     *
     *  Section 2: "Two adjacent strings, separated only by blanks, remarks, or
     *  card image boundaries, are automatically concatenated." A long WHERE
     *  comparand is written that way whenever it will not fit in 72 columns. */
    Token scan_string(int col) {
        const int line_no = current_line_no_;
        std::string body = scan_string_body();
        while (skip_to_significant() && line_[line_pos_] == '"')
            body += scan_string_body();
        return Token(STRTOK, std::move(body), line_no, col);
    }

    /** One quoted run, with "" standing for one quotation mark. An unterminated
     *  string ends at the card boundary rather than swallowing the rest of the
     *  file — which is also the manual's rule for a string that runs past
     *  column 72. */
    std::string scan_string_body() {
        ++line_pos_;  // opening quote
        std::string body;
        while (line_pos_ < line_.size()) {
            if (line_[line_pos_] == '"') {
                if (line_pos_ + 1 < line_.size() && line_[line_pos_ + 1] == '"') {
                    body.push_back('"');
                    line_pos_ += 2;
                    continue;
                }
                ++line_pos_;  // closing quote
                return body;
            }
            body.push_back(line_[line_pos_]);
            ++line_pos_;
        }
        return body;
    }

    /** Advance over blanks, remarks and card boundaries; false at end of file.
     *  Nothing it skips carries meaning, so there is no position to restore
     *  when the caller does not like what it finds. */
    bool skip_to_significant() {
        while (true) {
            if (line_pos_ >= line_.size()) {
                if (!read_next_line()) return false;
                continue;
            }
            const unsigned char c = static_cast<unsigned char>(line_[line_pos_]);
            if (std::isspace(c)) {
                ++line_pos_;
                continue;
            }
            if (c == '%') {
                line_pos_ = line_.size();
                continue;
            }
            return true;
        }
    }
};

class General {
public:
    std::string filename;
    std::string source;
    /** Base name of what this run writes: `<base>.model.json`, `<base>.sql`.
     *  Taken from -o, or from the source file when -o is not given. */
    std::string outputBase;
    /** Who keeps the automatic subsets current in the generated schema. */
    dasdl_sql::Maintenance maintenance = dasdl_sql::Maintenance::Triggers;
    int syntaxErrorCount = 0;

    bool hasSyntaxErrors() const { return syntaxErrorCount > 0; }
    void addSyntaxError() { ++syntaxErrorCount; }
    void clearSyntaxErrors() { syntaxErrorCount = 0; }
};

// token/parser definitions are emitted by Trackway in the generated .cpp;
// general has no emitted definition, so define it here as an inline variable.
inline General general;
extern std::unique_ptr<Parser> parser;
extern Token token;

inline void SEND(const std::string& message) {
    std::cout << message << '\n';
}

inline void ERROR(const std::string& message) {
    general.addSyntaxError();
    if (parser) {
        const std::string& line = parser->current_line();
        const int ln = token.line > 0 ? token.line : parser->current_line_number();
        const std::string leader = std::to_string(ln) + " | ";
        std::cerr << leader << line << '\n';
        const int caret_pos = token.column > 1 ? (token.column - 1) : 0;
        std::cerr << std::string(leader.size() + static_cast<size_t>(caret_pos), ' ')
                  << "^\n";
    }
    std::cerr << "ERROR: " << message << '\n';
}

inline std::string get_source_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-s" || a == "--source") && i + 1 < argc) return argv[i + 1];
        if (a.rfind("--source=", 0) == 0) return a.substr(9);
    }
    if (argc >= 2 && argv[1][0] != '-') return argv[1];
    return {};
}

/** -o names a base, not a file: the extension of whatever is written is
 *  appended to it. */
/** --maintain=<triggers|runtime>: who keeps the automatic subsets current in
 *  the generated schema — the server, through triggers, or whatever writes to
 *  the data set tables. */
inline std::string get_maintain_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--maintain" && i + 1 < argc) return argv[i + 1];
        if (a.rfind("--maintain=", 0) == 0) return a.substr(11);
    }
    return {};
}

inline std::string get_output_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-o" || a == "--output") && i + 1 < argc) return argv[i + 1];
        if (a.rfind("--output=", 0) == 0) return a.substr(9);
    }
    return {};
}

/** The source's file name with its directory and extension removed: a
 *  description compiled from ../samples/ writes its model here, not there. */
inline std::string base_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.erase(dot);
    return base;
}

inline bool INITIALIZE(int argc, char** argv) {
    general.clearSyntaxErrors();
    general.filename = (argc > 0) ? argv[0] : "dasdl";
    general.source = get_source_arg(argc, argv);
    if (general.source.empty()) {
        std::cerr << "Usage: " << general.filename
                  << " -s <database.dasdl> [-o <base>] "
                     "[--maintain=triggers|runtime]\n";
        return true;
    }
    general.outputBase = get_output_arg(argc, argv);
    if (general.outputBase.empty()) general.outputBase = base_of(general.source);
    const std::string maintain = get_maintain_arg(argc, argv);
    if (maintain == "runtime") general.maintenance = dasdl_sql::Maintenance::Runtime;
    else if (!maintain.empty() && maintain != "triggers") {
        std::cerr << general.filename << ": --maintain takes 'triggers' or 'runtime', not '"
                  << maintain << "'\n";
        return true;
    }
    {
        std::ifstream probe(general.source);
        if (!probe.is_open()) {
            std::cerr << general.filename
                      << ": cannot open source file '" << general.source << "'\n";
            return true;
        }
    }
    parser = std::make_unique<Parser>(general.source);
    model.db.name = dasdl_model::toUpper(base_of(general.source));
    model.db.source = general.source;
    std::cout << "source: " << general.source << '\n';
    std::cout << "compiling\n";
    return false;
}

inline bool PROCESS() {
    if (general.hasSyntaxErrors()) {
        std::cout << "-- SYNTAX ERRORS --\n";
        return true;
    }
    std::cout << "database description compiled OK\n";

    model.finalize();
    model.dumpSummary(std::cout);

    const std::string jsonPath = general.outputBase + ".model.json";
    {
        std::ofstream json(jsonPath);
        if (json.is_open()) {
            model.writeJson(json);
            std::cout << "Model: " << jsonPath << " generated\n";
        } else {
            std::cerr << "warning: cannot write model file '" << jsonPath << "'\n";
        }
    }

    // The schema is written from the model in memory, not from the JSON: the
    // model is what the writer reads, and a round trip through a file would
    // only give it a second chance to disagree with itself.
    if (std::getenv("DASDL_NO_SQL")) return false;
    const std::string sqlPath = general.outputBase + ".sql";
    std::ofstream sql(sqlPath);
    if (!sql.is_open()) {
        std::cerr << "warning: cannot write schema file '" << sqlPath << "'\n";
        return false;
    }
    const int unmapped = dasdl_sql::write(sql, model.db, general.maintenance);
    std::cout << "Schema: " << sqlPath << " generated\n";
    if (unmapped)
        std::cout << "  " << unmapped
                  << " structure(s) the schema could not map; see the "
                     "-- unsupported: lines\n";
    return false;
}
