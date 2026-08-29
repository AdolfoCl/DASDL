#pragma once

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// dasdl_model.hpp — the database this compiler builds out of a DASDL source.
//
// The front end recognised a description; this is the model behind it, the way
// MCP/C74 builds a program model and generates from it. It is what
// `dasdl_sql.hpp` reads to write the MariaDB schema, and what the C74 runtime
// will read to know the shape of the data its DMSII verbs reach.
//
// One shape for everything
// ------------------------
// The grammar already noticed that a DASDL declaration is a name followed by a
// body, and that the same shape serves a top-level structure and a record
// entry:
//
//     <STRUCTURE DECLARATION> ::= IDNTOK [STRTOK] <STRUCTURE BODY>
//     <RECORD ENTRY>          ::= IDNTOK [<RENAME>] [STRTOK] [<STRUCTURE BODY>]
//
// So the model is one node type, `Node`, and one stack. A node's `kind` is
// blank until the body says what it was — a data set, a set, a remap, an item,
// a physical specification for something declared earlier, or nothing at all
// (an entry a remap carries over unchanged, or a bit of a FIELD). Nesting is
// the same story at every level: an embedded data set inside a record is a
// child node whose kind is "dataset", exactly like a top-level one.
//
// How the parser drives it
// ------------------------
// Semantic hooks in `dasdl.graph` call the builder below:
//
//   beginEntry(name)     a name was read — a declaration or a record entry
//   openScope()/closeScope()
//                        the '(' and ')' of a record description: what follows
//                        belongs to the entry just named
//   setKind/setType/...  the body says what the open entry is
//   endDeclaration()     the ';' of the database element list, which closes
//                        everything back to the top
//
// Every setter works on `owner()` — the open entry, or the enclosing one when
// no entry is open (the state right after a ')'). Outside any entry the
// setters are no-ops, so the parts of the source that declare no structure
// (OPTIONS, PARAMETERS, DEFAULTS) cannot leak into the last structure read.
// Those route their attributes through an explicit sink stack instead.
// ---------------------------------------------------------------------------

namespace dasdl_model {

/** JSON string escaping. */
inline std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

inline std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

/** One `NAME = value` pair. Attribute names in DASDL are open-ended — every
 *  physical option, run-time parameter and database option in the manual lands
 *  here — so they are carried as written rather than enumerated. The value is
 *  the run of words that followed the '=' (`75 BLOCKS`, `4 + 1 PER RANDOM
 *  USER`), joined with single spaces. */
struct Attribute {
    std::string name;
    std::string value;
    int line = 0;
};

/** One item of a set's key list, or of its DATA(...) list. */
struct KeyRef {
    std::string name;
    std::string order;   // ASCENDING / DESCENDING; empty when not written
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

// Node kinds. A node is one of these once its body has been read.
//   ""                the body said nothing yet
//   "dataset"         DATA SET, in any of its four spellings
//   "set"/"subset"/"access"
//   "remap"           REMAPS x — of a data set record or of the global items
//   "logical-database"
//   "item"            an item description: ALPHA, NUMBER, GROUP, POPULATION...
//   "physical"        a physical specification for a structure declared above
//   "carried"         a name on its own: a remap entry taken over unchanged
//   "field-bit"       the same, resolved by finalize() when the parent is FIELD
//   "filler"          FILLER SIZE n
//   "format"          one labelled part of a variable-format record
//   "ldb-entry"       a structure named by a logical database
//   "ldb-set"         a set named inside a logical database entry
//   "of"              the bare `OF name` body

struct Node {
    // ---- identity ---------------------------------------------------------
    std::string name;
    std::string kind;
    std::string comment;      // the quoted string a declaration may carry
    std::string renamedFrom;  // remap / logical database: NEW = OLD
    int line = 0;

    // ---- item facts -------------------------------------------------------
    std::string type;         // alpha|number|real|boolean|field|group|population|count
    int size = 0;             // ALPHA characters, NUMBER digits, FIELD bits
    int scale = 0;            // digits after the decimal point
    bool isSigned = false;    // NUMBER(S7,2)
    bool hasSize = false;     // NUMBER on its own carries no size
    int occurs = 0;
    std::string dependingOn;
    bool isVirtual = false;
    std::string virtualExpr;
    bool hasNull = false;     std::string nullValue;
    bool hasInitial = false;  std::string initialValue;
    bool required = false;
    bool readOnly = false;
    bool hidden = false;
    int fillerSize = 0;

    // ---- structure facts --------------------------------------------------
    std::string datasetKind;  // COMPACT / DIRECT / ORDERED / RANDOM / RESTART / ...
    std::string target;       // SET OF x, ACCESS TO x, REMAPS x, POPULATION OF x
    std::vector<KeyRef> keys;
    std::vector<KeyRef> dataItems;   // DATA(...) carried in the index entry
    std::string setType;      // bit vector | list | unordered list | index random | ...
    bool duplicatesSeen = false;
    bool duplicatesAllowed = false;
    std::string duplicatesOption;    // FIRST / LAST / KEYCHANGEOK
    std::string whereText;
    std::string selectText;
    std::string verifyText;
    bool globalData = false;  // a remap of the database's global items
    std::string setsMode;     // logical database entry: ALL | NONE | LIST
    std::string guardFile;
    int formatNumber = -1;    // variable-format part label

    std::vector<Attribute> attributes;
    std::vector<NodePtr> children;
    Node* parent = nullptr;

    // ---- filled by finalize() --------------------------------------------
    /** The structure a set/subset/access/remap points at, when it resolves. */
    Node* targetNode = nullptr;
    bool unresolved = false;

    const Node* child(const std::string& n) const {
        for (const auto& c : children)
            if (toUpper(c->name) == toUpper(n)) return c.get();
        return nullptr;
    }
};

/** A code file specification: ACCESSROUTINES = (USER)NAME/NODE ON FAMILY. */
struct CodeFile {
    std::string name;       // ACCESSROUTINES, DMSUPPORT, ...
    std::string usercode;
    std::string title;      // nodes joined with '/'
    std::string family;
};

/** A per-structure block inside DEFAULTS: `DATA SET (...)`, `ALPHA (...)`. */
struct StructureDefault {
    std::string kind;
    std::vector<Attribute> attributes;
};

struct Database {
    std::string name;          // the source file's base name; DASDL never names itself
    std::string source;
    std::string modelName;     // MODEL x
    bool isUpdate = false;
    std::string updateName;    // UPDATE [x]

    std::vector<Attribute> options;
    std::vector<Attribute> parameters;
    std::vector<Attribute> defaults;
    std::vector<Attribute> controlFile;
    std::vector<Attribute> auditTrail;
    std::deque<StructureDefault> structureDefaults;
    std::vector<CodeFile> codeFiles;

    std::vector<NodePtr> nodes;

    const Node* find(const std::string& n) const {
        const std::string want = toUpper(n);
        for (const auto& c : nodes)
            if (toUpper(c->name) == want) return c.get();
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// The builder
// ---------------------------------------------------------------------------

class ModelBuilder {
public:
    Database db;

    // ---- the entry stack --------------------------------------------------

    /** A name was read: a structure declaration, or an entry inside a record
     *  description. The previous sibling simply stops being current — nodes
     *  are never closed, only left behind. */
    void beginEntry(const std::string& name, int line) {
        auto node = std::make_unique<Node>();
        node->name = toUpper(name);
        node->line = line;
        node->parent = scope_.empty() ? nullptr : scope_.back();
        Node* raw = node.get();
        container().push_back(std::move(node));
        current_ = raw;
        pending_ = Pending::None;
    }

    /** FILLER SIZE n: an entry with no name, and nothing but its width. */
    void beginFiller(int line) {
        beginEntry("FILLER", line);
        current_->kind = "filler";
    }
    void setFillerSize(int bits) { if (current_) current_->fillerSize = bits; }

    /** The '(' of a record description: what follows belongs to the entry just
     *  named. Nothing else opens a scope — a physical option list is a flat
     *  list of attributes, not a set of children. */
    void openScope() {
        scope_.push_back(current_);
        current_ = nullptr;
    }

    /** The matching ')'. The entry that owned the description becomes current
     *  again, which is what lets `G GROUP (...) REQUIRED` land its attribute on
     *  the group rather than on its last child. */
    void closeScope() {
        if (scope_.empty()) return;
        current_ = scope_.back();
        scope_.pop_back();
        pending_ = Pending::None;
    }

    /** The ';' that ends a database element. Everything the element opened is
     *  closed here, so the next element starts clean however deep the last one
     *  went — and however badly it ended, since error recovery resumes on this
     *  same token. */
    void endDeclaration() {
        endGuardFile();
        scope_.clear();
        current_ = nullptr;
        sinks_.clear();
        pending_ = Pending::None;
        exprSink_ = nullptr;
        titleSink_ = nullptr;
        attrSink_ = nullptr;
        keysAreData_ = false;
        negated_ = false;
        pushedByOptions_.clear();
        pendingDefaultKind_.clear();
    }

    // ---- what the body says the entry was ---------------------------------

    void setKind(const std::string& kind) { if (Node* n = owner()) n->kind = kind; }

    /** A physical option list with no other body: `PERSONNEL (KIND = PACK)`
     *  names a structure declared earlier and gives it its physical options.
     *  Anything that already knows what it is keeps its kind. */
    void notePhysicalOptions() {
        if (!sinks_.empty()) return;
        if (Node* n = owner(); n && n->kind.empty()) n->kind = "physical";
    }

    void setDatasetKind(const std::string& word) {
        if (Node* n = owner()) n->datasetKind = toUpper(word);
    }

    /** DATA / DATA-SET / DATASET. The same word opens a per-structure block
     *  inside DEFAULTS, where there is no structure to name. */
    void noteDataSetWord() {
        if (!sinks_.empty()) { pendingDefaultKind_ = "DATA SET"; return; }
        setKind("dataset");
    }

    void setTarget(const std::string& name) {
        if (Node* n = owner()) n->target = toUpper(name);
    }

    void setComment(const std::string& text) {
        if (Node* n = owner()) n->comment = text;
    }

    /** `NEW = OLD` in a remap or a logical database. The grammar reads it as a
     *  name and an optional '= name'; the manual writes the new name first, so
     *  what the second name is, is the old one. */
    void setRename(const std::string& oldName) {
        if (Node* n = owner()) n->renamedFrom = toUpper(oldName);
    }

    // ---- items ------------------------------------------------------------

    void setItemType(const std::string& type) {
        if (Node* n = owner()) {
            n->type = type;
            if (n->kind.empty() || n->kind == "carried") n->kind = "item";
        }
    }

    void setSize(int size) {
        if (Node* n = owner()) { n->size = size; n->hasSize = true; }
    }

    /** The signed form `NUMBER(S7,2)`: the S is a letter, so the size reaches
     *  the parser as an identifier rather than an integer. */
    void setSignedSize(const std::string& word) {
        Node* n = owner();
        if (!n) return;
        const std::string w = toUpper(word);
        size_t i = 0;
        if (i < w.size() && w[i] == 'S') { n->isSigned = true; ++i; }
        int digits = 0;
        bool any = false;
        for (; i < w.size() && std::isdigit(static_cast<unsigned char>(w[i])); ++i) {
            digits = digits * 10 + (w[i] - '0');
            any = true;
        }
        if (any) { n->size = digits; n->hasSize = true; }
    }

    void setScale(int scale) { if (Node* n = owner()) n->scale = scale; }
    void setOccurs(int times) { if (Node* n = owner()) n->occurs = times; }
    void setDepending(const std::string& name) {
        if (Node* n = owner()) n->dependingOn = toUpper(name);
    }
    void markVirtual() { if (Node* n = owner()) n->isVirtual = true; }
    void markRequired() { if (Node* n = owner()) n->required = true; }
    void markReadOnly() { if (Node* n = owner()) n->readOnly = true; }

    /** NULL IS ... and INITIALVALUE IS ... share <ITEM VALUE>, so the clause
     *  that opened says which of the two the next value belongs to. */
    void expectNullValue() { pending_ = Pending::Null; }
    void expectInitialValue() { pending_ = Pending::Initial; }

    void setPendingValue(const std::string& text) {
        Node* n = owner();
        if (!n) return;
        if (pending_ == Pending::Null) { n->hasNull = true; n->nullValue = toUpper(text); }
        else if (pending_ == Pending::Initial) { n->hasInitial = true; n->initialValue = toUpper(text); }
        pending_ = Pending::None;
    }

    // ---- sets -------------------------------------------------------------

    /** KEY IS (A, B DESCENDING). The DATA(...) list of an index entry reaches
     *  the same key-item entity, so the DATA keyword redirects it. */
    void beginDataItems() { keysAreData_ = true; }
    void endDataItems() { keysAreData_ = false; }

    void addKey(const std::string& name) {
        Node* n = owner();
        if (!n) return;
        (keysAreData_ ? n->dataItems : n->keys).push_back(KeyRef{toUpper(name), {}});
    }

    void setKeyOrder(const std::string& order) {
        Node* n = owner();
        if (!n) return;
        auto& list = keysAreData_ ? n->dataItems : n->keys;
        if (!list.empty()) list.back().order = toUpper(order);
    }

    void setSetType(const std::string& type) { if (Node* n = owner()) n->setType = type; }

    /** `NO DUPLICATES` is drawn as an optional NO in front of the word, so the
     *  NO is remembered and the word that follows decides. */
    void noteNo() { negated_ = true; }
    void noteDuplicates() { setDuplicates(!negated_); negated_ = false; }

    void setDuplicates(bool allowed) {
        if (Node* n = owner()) { n->duplicatesSeen = true; n->duplicatesAllowed = allowed; }
    }
    void setDuplicatesOption(const std::string& word) {
        if (Node* n = owner()) n->duplicatesOption = toUpper(word);
    }

    // ---- expressions ------------------------------------------------------
    //
    // WHERE conditions, SELECT/VERIFY predicates and the value of a virtual
    // item are all <BOOLEAN EXPRESSION>, and none of them is interpreted here:
    // they are kept as the run of tokens that spelled them, which is what a
    // generator needs to write the same condition in SQL.

    void beginWhere()   { openExpr(&ownerExpr(&Node::whereText)); }
    void beginSelect()  { openExpr(&ownerExpr(&Node::selectText)); }
    void beginVerify()  { openExpr(&ownerExpr(&Node::verifyText)); }
    void beginSelectOrVerify(const std::string& word) {
        if (toUpper(word) == "SELECT") beginSelect(); else beginVerify();
    }
    void beginVirtual() { openExpr(&ownerExpr(&Node::virtualExpr)); }

    void addExprToken(const std::string& text) {
        if (!exprSink_) return;
        if (!exprSink_->empty()) *exprSink_ += ' ';
        *exprSink_ += toUpper(text);
    }

    void addExprString(const std::string& text) {
        if (!exprSink_) return;
        if (!exprSink_->empty()) *exprSink_ += ' ';
        *exprSink_ += '"' + text + '"';
    }

    // ---- attributes -------------------------------------------------------

    void pushSink(std::vector<Attribute>* sink) { sinks_.push_back(sink); }
    void popSink() { if (!sinks_.empty()) sinks_.pop_back(); }

    void beginOptions()     { pushSink(&db.options); }
    void beginParameters()  { pushSink(&db.parameters); }
    void beginDefaults()    { pushSink(&db.defaults); }
    void beginControlFile() { pushSink(&db.controlFile); }
    void beginAuditTrail()  { pushSink(&db.auditTrail); }

    /** A per-structure block inside DEFAULTS. Its kind word is read first and
     *  the '(' that follows is what opens the block. */
    void noteDefaultKind(const std::string& word) { pendingDefaultKind_ = toUpper(word); }

    /** The '(' of a physical option list. Inside DEFAULTS it opens the block
     *  the kind word announced; everywhere else the attributes belong to
     *  whatever the enclosing sink or the open entry is. */
    void openPhysicalOptions() {
        if (!sinks_.empty() && !pendingDefaultKind_.empty()) {
            db.structureDefaults.push_back(StructureDefault{pendingDefaultKind_, {}});
            pendingDefaultKind_.clear();
            pushSink(&db.structureDefaults.back().attributes);
            pushedByOptions_.push_back(true);
            return;
        }
        notePhysicalOptions();
        pushedByOptions_.push_back(false);
    }

    void closePhysicalOptions() {
        if (pushedByOptions_.empty()) return;
        if (pushedByOptions_.back()) popSink();
        pushedByOptions_.pop_back();
    }

    void beginAttribute(const std::string& name, int line) {
        std::vector<Attribute>* sink = attributeSink();
        if (!sink) { attrSink_ = nullptr; return; }
        sink->push_back(Attribute{toUpper(name), {}, line});
        // Held as list-and-index rather than as a pointer: the next attribute
        // pushed into the same list can move it in memory.
        attrSink_ = sink;
        attrIndex_ = sink->size() - 1;
    }

    void addAttributeValue(const std::string& word) {
        if (Attribute* a = openAttribute()) {
            if (!a->value.empty()) a->value += ' ';
            a->value += toUpper(word);
        }
    }

    void addAttributeString(const std::string& text) {
        if (Attribute* a = openAttribute()) {
            if (!a->value.empty()) a->value += ' ';
            a->value += '"' + text + '"';
        }
    }

    // ---- database elements ------------------------------------------------

    void setModelName(const std::string& name) { db.modelName = toUpper(name); }
    void markUpdate() { db.isUpdate = true; }
    void setUpdateName(const std::string& name) { db.updateName = toUpper(name); }

    void beginCodeFile(const std::string& name) {
        db.codeFiles.push_back(CodeFile{toUpper(name), {}, {}, {}});
        titleSink_ = &db.codeFiles.back();
    }

    /** GUARDFILE on a logical database reaches the same file-title entity as a
     *  code file, so the title is collected into a scratch record and handed to
     *  the node when the declaration ends. */
    void beginGuardFile() {
        guard_ = CodeFile{};
        titleSink_ = &guard_;
        guardOpen_ = true;
    }

    void setUsercode(const std::string& name) { if (titleSink_) titleSink_->usercode = toUpper(name); }

    void addTitleNode(const std::string& name) {
        if (!titleSink_) return;
        if (!titleSink_->title.empty()) titleSink_->title += '/';
        titleSink_->title += toUpper(name);
    }

    void setFamily(const std::string& name) { if (titleSink_) titleSink_->family = toUpper(name); }

    /** Called when the logical database declaration is done with its title. */
    void endGuardFile() {
        if (!guardOpen_) return;
        if (Node* n = owner()) {
            n->guardFile = guard_.usercode.empty() ? guard_.title
                                                   : "(" + guard_.usercode + ")" + guard_.title;
            if (!guard_.family.empty()) n->guardFile += " ON " + guard_.family;
        }
        guardOpen_ = false;
        titleSink_ = nullptr;
    }

    void setSetsMode(const std::string& mode) { if (Node* n = owner()) n->setsMode = toUpper(mode); }

    /** One labelled part of a variable-format record. The parts are siblings
     *  under the record they redescribe, so a second part hangs off the same
     *  host as the first rather than off the first itself. */
    void beginFormat(int label, int line) {
        Node* host = current_;
        if (host && host->kind == "format") host = host->parent;
        if (!host) host = owner();
        if (!host) return;
        auto node = std::make_unique<Node>();
        node->name = "FORMAT-" + std::to_string(label);
        node->kind = "format";
        node->formatNumber = label;
        node->line = line;
        node->parent = host;
        current_ = node.get();
        host->children.push_back(std::move(node));
    }

    // ---- finalize ---------------------------------------------------------

    /** Resolve what the source only named: the structure a set or remap points
     *  at, and the entries a FIELD declaration holds. Nothing here rejects a
     *  description — an unresolved name is recorded and reported, because a
     *  logical database may name structures a fragment of the source did not
     *  declare. */
    void finalize() {
        for (auto& n : db.nodes) resolve(*n, nullptr);
    }

    int unresolvedCount() const { return unresolved_; }

    // ---- output -----------------------------------------------------------

    void writeJson(std::ostream& os) const {
        os << "{\n";
        os << "  \"database\": " << jsonStr(db.name) << ",\n";
        os << "  \"source\": " << jsonStr(db.source) << ",\n";
        if (!db.modelName.empty()) os << "  \"model\": " << jsonStr(db.modelName) << ",\n";
        if (db.isUpdate) {
            os << "  \"update\": " << (db.updateName.empty() ? std::string("true")
                                                             : jsonStr(db.updateName)) << ",\n";
        }
        writeAttrs(os, "options", db.options);
        writeAttrs(os, "parameters", db.parameters);
        writeAttrs(os, "defaults", db.defaults);
        writeAttrs(os, "controlFile", db.controlFile);
        writeAttrs(os, "auditTrail", db.auditTrail);

        os << "  \"structureDefaults\": [";
        for (size_t i = 0; i < db.structureDefaults.size(); ++i) {
            os << (i ? ",\n    " : "\n    ") << "{\"kind\": "
               << jsonStr(db.structureDefaults[i].kind) << ", \"attributes\": ";
            writeAttrArray(os, db.structureDefaults[i].attributes);
            os << "}";
        }
        os << (db.structureDefaults.empty() ? "" : "\n  ") << "],\n";

        os << "  \"codeFiles\": [";
        for (size_t i = 0; i < db.codeFiles.size(); ++i) {
            const CodeFile& f = db.codeFiles[i];
            os << (i ? ",\n    " : "\n    ")
               << "{\"name\": " << jsonStr(f.name)
               << ", \"usercode\": " << jsonStr(f.usercode)
               << ", \"title\": " << jsonStr(f.title)
               << ", \"family\": " << jsonStr(f.family) << "}";
        }
        os << (db.codeFiles.empty() ? "" : "\n  ") << "],\n";

        os << "  \"structures\": ";
        writeNodes(os, db.nodes, 2);
        os << "\n}\n";
    }

    void dumpSummary(std::ostream& os) const {
        int datasets = 0, sets = 0, remaps = 0, ldbs = 0, items = 0;
        for (const auto& n : db.nodes) count(*n, datasets, sets, remaps, ldbs, items);
        os << "Structures: " << datasets << " data set(s), "
           << sets << " set(s), " << remaps << " remap(s), "
           << ldbs << " logical database(s), " << items << " item(s)\n";
        if (unresolved_)
            os << "warning: " << unresolved_ << " name(s) referenced but not declared here\n";
    }

private:
    enum class Pending { None, Null, Initial };

    Node* current_ = nullptr;
    std::vector<Node*> scope_;
    std::vector<std::vector<Attribute>*> sinks_;
    std::vector<bool> pushedByOptions_;
    std::vector<Attribute>* attrSink_ = nullptr;
    size_t attrIndex_ = 0;
    Pending pending_ = Pending::None;
    bool keysAreData_ = false;
    bool negated_ = false;
    std::string* exprSink_ = nullptr;
    std::string pendingDefaultKind_;
    CodeFile* titleSink_ = nullptr;
    CodeFile guard_;
    bool guardOpen_ = false;
    int unresolved_ = 0;

    /** The node a setter works on: the open entry, or the one that owns the
     *  description just closed. */
    Node* owner() {
        if (current_) return current_;
        return scope_.empty() ? nullptr : scope_.back();
    }

    std::vector<NodePtr>& container() {
        if (scope_.empty() || scope_.back() == nullptr) return db.nodes;
        return scope_.back()->children;
    }

    Attribute* openAttribute() {
        if (!attrSink_ || attrIndex_ >= attrSink_->size()) return nullptr;
        return &(*attrSink_)[attrIndex_];
    }

    std::vector<Attribute>* attributeSink() {
        if (!sinks_.empty()) return sinks_.back();
        if (Node* n = owner()) return &n->attributes;
        return nullptr;
    }

    std::string& ownerExpr(std::string Node::*field) {
        static std::string sink;   // nowhere to put it: an expression with no owner
        Node* n = owner();
        return n ? n->*field : sink;
    }

    void openExpr(std::string* sink) { exprSink_ = sink; sink->clear(); }

    void resolve(Node& n, Node* parent) {
        n.parent = parent;
        if (n.kind.empty()) n.kind = "carried";
        // A name on its own inside a FIELD declaration is one of its bits, not
        // an entry carried into a remap. Only the parent can tell.
        if (n.kind == "carried" && parent && parent->type == "field") n.kind = "field-bit";
        // HIDDEN and the two-word REQUIRED ALL / READONLY ALL arrive as plain
        // attributes: the manual has hundreds of attribute names and the
        // grammar spells out only the ones whose shape is more than a name.
        for (const Attribute& a : n.attributes) {
            if (a.name == "HIDDEN") n.hidden = true;
            else if (a.name == "READONLY") n.readOnly = true;
            else if (a.name == "REQUIRED") n.required = true;
        }
        if (!n.target.empty()) {
            n.targetNode = lookup(n.target, parent);
            // A remap that names nothing declared here redescribes the global
            // items: its target is the database, and a DASDL source never
            // names the database it describes — the compilation does.
            if (!n.targetNode && n.kind == "remap") n.globalData = true;
            else if (!n.targetNode) { n.unresolved = true; ++unresolved_; }
        }
        for (auto& c : n.children) resolve(*c, &n);
    }

    /** A set declared inside a record reaches the data set embedded beside it;
     *  one declared at the top reaches a top-level structure. Search outward. */
    Node* lookup(const std::string& name, Node* from) {
        for (Node* scope = from; scope; scope = scope->parent) {
            for (auto& c : scope->children)
                if (toUpper(c->name) == name) return c.get();
        }
        for (auto& c : db.nodes)
            if (toUpper(c->name) == name) return c.get();
        return nullptr;
    }

    static void count(const Node& n, int& ds, int& st, int& rm, int& ldb, int& it) {
        if (n.kind == "dataset") ++ds;
        else if (n.kind == "set" || n.kind == "subset" || n.kind == "access") ++st;
        else if (n.kind == "remap") ++rm;
        else if (n.kind == "logical-database") ++ldb;
        else if (n.kind == "item") ++it;
        for (const auto& c : n.children) count(*c, ds, st, rm, ldb, it);
    }

    static void writeAttrArray(std::ostream& os, const std::vector<Attribute>& list) {
        os << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            os << (i ? ", " : "") << "{\"name\": " << jsonStr(list[i].name)
               << ", \"value\": " << jsonStr(list[i].value) << "}";
        }
        os << "]";
    }

    static void writeAttrs(std::ostream& os, const char* label,
                           const std::vector<Attribute>& list) {
        os << "  " << jsonStr(label) << ": ";
        writeAttrArray(os, list);
        os << ",\n";
    }

    static void writeNodes(std::ostream& os, const std::vector<NodePtr>& list, int indent) {
        const std::string pad(static_cast<size_t>(indent), ' ');
        if (list.empty()) { os << "[]"; return; }
        os << "[\n";
        for (size_t i = 0; i < list.size(); ++i) {
            writeNode(os, *list[i], indent + 2);
            os << (i + 1 < list.size() ? ",\n" : "\n");
        }
        os << pad << "]";
    }

    static void writeNode(std::ostream& os, const Node& n, int indent) {
        const std::string pad(static_cast<size_t>(indent), ' ');
        const std::string in(static_cast<size_t>(indent) + 2, ' ');
        os << pad << "{\n";
        os << in << "\"name\": " << jsonStr(n.name) << ",\n";
        os << in << "\"kind\": " << jsonStr(n.kind) << ",\n";
        os << in << "\"line\": " << n.line;
        auto str = [&](const char* k, const std::string& v) {
            if (!v.empty()) os << ",\n" << in << jsonStr(k) << ": " << jsonStr(v);
        };
        auto num = [&](const char* k, int v) {
            if (v) os << ",\n" << in << jsonStr(k) << ": " << v;
        };
        auto flag = [&](const char* k, bool v) {
            if (v) os << ",\n" << in << jsonStr(k) << ": true";
        };
        str("type", n.type);
        if (n.hasSize) os << ",\n" << in << "\"size\": " << n.size;
        num("scale", n.scale);
        flag("signed", n.isSigned);
        num("occurs", n.occurs);
        str("dependingOn", n.dependingOn);
        flag("virtual", n.isVirtual);
        str("virtualExpr", n.virtualExpr);
        if (n.hasNull) os << ",\n" << in << "\"null\": " << jsonStr(n.nullValue);
        if (n.hasInitial) os << ",\n" << in << "\"initialValue\": " << jsonStr(n.initialValue);
        flag("required", n.required);
        flag("readOnly", n.readOnly);
        flag("hidden", n.hidden);
        num("fillerSize", n.fillerSize);
        str("datasetKind", n.datasetKind);
        str("target", n.target);
        flag("globalData", n.globalData);
        flag("unresolved", n.unresolved);
        str("comment", n.comment);
        str("renamedFrom", n.renamedFrom);
        str("setType", n.setType);
        if (n.duplicatesSeen)
            os << ",\n" << in << "\"duplicates\": " << (n.duplicatesAllowed ? "true" : "false");
        str("duplicatesOption", n.duplicatesOption);
        str("where", n.whereText);
        str("select", n.selectText);
        str("verify", n.verifyText);
        str("setsMode", n.setsMode);
        str("guardFile", n.guardFile);
        num("format", n.formatNumber > 0 ? n.formatNumber : 0);
        if (!n.keys.empty()) { os << ",\n" << in << "\"keys\": "; writeKeys(os, n.keys); }
        if (!n.dataItems.empty()) { os << ",\n" << in << "\"data\": "; writeKeys(os, n.dataItems); }
        if (!n.attributes.empty()) {
            os << ",\n" << in << "\"attributes\": ";
            writeAttrArray(os, n.attributes);
        }
        if (!n.children.empty()) {
            os << ",\n" << in << "\"items\": ";
            writeNodes(os, n.children, indent + 2);
        }
        os << "\n" << pad << "}";
    }

    static void writeKeys(std::ostream& os, const std::vector<KeyRef>& keys) {
        os << "[";
        for (size_t i = 0; i < keys.size(); ++i) {
            os << (i ? ", " : "") << "{\"name\": " << jsonStr(keys[i].name);
            if (!keys[i].order.empty()) os << ", \"order\": " << jsonStr(keys[i].order);
            os << "}";
        }
        os << "]";
    }
};

}  // namespace dasdl_model

/** The one model this compiler builds; the graph's semantic hooks call it. */
inline dasdl_model::ModelBuilder model;
