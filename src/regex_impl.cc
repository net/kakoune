#include "regex_impl.hh"

#include "exception.hh"
#include "string.hh"
#include "unicode.hh"
#include "unit_tests.hh"
#include "utf8.hh"
#include "utf8_iterator.hh"
#include "string_utils.hh"
#include "vector.hh"

#include <cstring> 

namespace Kakoune
{

struct ParsedRegex
{
    enum Op : char
    {
        Literal,
        AnyChar,
        Matcher,
        Sequence,
        Alternation,
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        SubjectBegin,
        SubjectEnd,
        ResetStart,
        LookAhead,
        NegativeLookAhead,
        LookBehind,
        NegativeLookBehind,
    };

    struct Quantifier
    {
        enum Type : char
        {
            One,
            Optional,
            RepeatZeroOrMore,
            RepeatOneOrMore,
            RepeatMinMax,
        };
        Type type = One;
        bool greedy = true;
        int min = -1, max = -1;

        bool allows_none() const
        {
            return type == Quantifier::Optional or
                   type == Quantifier::RepeatZeroOrMore or
                  (type == Quantifier::RepeatMinMax and min <= 0);
        }

        bool allows_infinite_repeat() const
        {
            return type == Quantifier::RepeatZeroOrMore or
                   type == Quantifier::RepeatOneOrMore or
                  (type == Quantifier::RepeatMinMax and max == -1);
        };
    };

    struct AstNode;
    using AstNodePtr = std::unique_ptr<AstNode>;

    struct AstNode
    {
        Op op;
        bool ignore_case;
        Codepoint value;
        Quantifier quantifier;
        Vector<AstNodePtr, MemoryDomain::Regex> children;
    };

    AstNodePtr ast;
    size_t capture_count;
    Vector<std::function<bool (Codepoint)>, MemoryDomain::Regex> matchers;
};

// Recursive descent parser based on naming used in the ECMAScript
// standard, although the syntax is not fully compatible.
struct RegexParser
{
    RegexParser(StringView re)
        : m_regex{re}, m_pos{re.begin(), re}
    {
        m_parsed_regex.capture_count = 1;
        m_parsed_regex.ast = disjunction(0);
    }

    ParsedRegex get_parsed_regex() { return std::move(m_parsed_regex); }

    static ParsedRegex parse(StringView re) { return RegexParser{re}.get_parsed_regex(); }

private:
    struct InvalidPolicy
    {
        Codepoint operator()(Codepoint cp) { throw regex_error{"Invalid utf8 in regex"}; }
    };

    using Iterator = utf8::iterator<const char*, Codepoint, int, InvalidPolicy>;
    using AstNodePtr = ParsedRegex::AstNodePtr;

    AstNodePtr disjunction(unsigned capture = -1)
    {
        AstNodePtr node = alternative();
        if (at_end() or *m_pos != '|')
        {
            node->value = capture;
            return node;
        }

        AstNodePtr res = new_node(ParsedRegex::Alternation);
        res->value = capture;
        res->children.push_back(std::move(node));
        do
        {
            ++m_pos;
            res->children.push_back(alternative());
        }
        while (not at_end() and *m_pos == '|');
        return res;
    }

    AstNodePtr alternative(ParsedRegex::Op op = ParsedRegex::Sequence)
    {
        AstNodePtr res = new_node(op);
        while (auto node = term())
            res->children.push_back(std::move(node));
        return res;
    }

    AstNodePtr term()
    {
        while (modifiers()) // read all modifiers
        {}
        if (auto node = assertion())
            return node;
        if (auto node = atom())
        {
            node->quantifier = quantifier();
            return node;
        }
        return nullptr;
    }

    bool peek(StringView expected) const
    {
        auto it = m_pos;
        for (Iterator expected_it{expected.begin(), expected}; expected_it != expected.end(); ++expected_it)
        {
            if (it == m_regex.end() or *it++ != *expected_it)
                return false;
        }
        return true;
    }

    bool modifiers()
    {
        if (peek("(?i)"))
        {
            m_ignore_case = true;
            m_pos += 4;
            return true;
        }
        if (peek("(?I)"))
        {
            m_ignore_case = false;
            m_pos += 4;
            return true;
        }
        return false;
    }

    AstNodePtr assertion()
    {
        if (at_end())
            return nullptr;

        switch (*m_pos)
        {
            case '^': ++m_pos; return new_node(ParsedRegex::LineStart);
            case '$': ++m_pos; return new_node(ParsedRegex::LineEnd);
            case '\\':
                if (m_pos+1 == m_regex.end())
                    return nullptr;
                switch (*(m_pos+1))
                {
                    case 'b': m_pos += 2; return new_node(ParsedRegex::WordBoundary);
                    case 'B': m_pos += 2; return new_node(ParsedRegex::NotWordBoundary);
                    case 'A': m_pos += 2; return new_node(ParsedRegex::SubjectBegin);
                    case 'z': m_pos += 2; return new_node(ParsedRegex::SubjectEnd);
                    case 'K': m_pos += 2; return new_node(ParsedRegex::ResetStart);
                }
                break;
            case '(':
            {
                Optional<ParsedRegex::Op> lookaround_op;
                constexpr struct { StringView prefix; ParsedRegex::Op op; } lookarounds[] = {
                    { "(?=", ParsedRegex::LookAhead },
                    { "(?!", ParsedRegex::NegativeLookAhead },
                    { "(?<=", ParsedRegex::LookBehind },
                    { "(?<!", ParsedRegex::NegativeLookBehind }
                };
                for (auto& lookaround : lookarounds)
                {
                    if (peek(lookaround.prefix))
                    {
                        lookaround_op = lookaround.op;
                        m_pos += (int)lookaround.prefix.char_length();
                        break;
                    }
                }
                if (not lookaround_op)
                        return nullptr;

                AstNodePtr lookaround = alternative(*lookaround_op);
                if (at_end() or *m_pos++ != ')')
                    parse_error("unclosed parenthesis");

                validate_lookaround(lookaround);
                return lookaround;
            }
        }
        return nullptr;
    }

    AstNodePtr atom()
    {
        if (at_end())
            return nullptr;

        const Codepoint cp = *m_pos;
        switch (cp)
        {
            case '.': ++m_pos; return new_node(ParsedRegex::AnyChar);
            case '(':
            {
                ++m_pos;
                bool capture = true;
                if (peek("?:"))
                {
                    capture = false;
                    m_pos += 2;
                }

                AstNodePtr content = disjunction(capture ? m_parsed_regex.capture_count++ : -1);
                if (at_end() or *m_pos++ != ')')
                    parse_error("unclosed parenthesis");
                return content;
            }
            case '\\':
                ++m_pos;
                return atom_escape();
            case '[':
                ++m_pos;
                return character_class();
            case '|': case ')':
                return nullptr;
            default:
                if (contains("^$.*+?[]{}", cp))
                    parse_error(format("unexpected '{}'", cp));
                ++m_pos;
                return new_node(ParsedRegex::Literal, cp);
        }
    }

    AstNodePtr atom_escape()
    {
        const Codepoint cp = *m_pos++;

        if (cp == 'Q')
        {
            auto escaped_sequence = new_node(ParsedRegex::Sequence);
            constexpr StringView end_mark{"\\E"};
            auto quote_end = std::search(m_pos.base(), m_regex.end(), end_mark.begin(), end_mark.end());
            while (m_pos != quote_end)
                escaped_sequence->children.push_back(new_node(ParsedRegex::Literal, *m_pos++));
            if (quote_end != m_regex.end())
                m_pos += 2;

            return escaped_sequence;
        }

        // CharacterClassEscape
        auto class_it = find_if(character_class_escapes,
                                [cp = to_lower(cp)](auto& c) { return c.cp == cp; });
        if (class_it != std::end(character_class_escapes))
        {
            auto matcher_id = m_parsed_regex.matchers.size();
            m_parsed_regex.matchers.push_back(
                [ctype = class_it->ctype ? wctype(class_it->ctype) : (wctype_t)0,
                 chars = class_it->additional_chars, neg = is_upper(cp)] (Codepoint cp) {
                    return ((ctype != 0 and iswctype(cp, ctype)) or contains(chars, cp)) != neg;
                });
            return new_node(ParsedRegex::Matcher, matcher_id);
        }

        // CharacterEscape
        for (auto& control : control_escapes)
        {
            if (control.name == cp)
                return new_node(ParsedRegex::Literal, control.value);
        }

        // TOOD: \c..., \0..., '\0x...', \u...

        if (contains("^$\\.*+?()[]{}|", cp)) // SyntaxCharacter
            return new_node(ParsedRegex::Literal, cp);
        parse_error(format("unknown atom escape '{}'", cp));
    }

    struct CharRange { Codepoint min, max; };

    void normalize_ranges(Vector<CharRange, MemoryDomain::Regex>& ranges)
    {
        if (ranges.empty())
            return;

        // Sort ranges so that we can use binary search
        std::sort(ranges.begin(), ranges.end(),
                  [](auto& lhs, auto& rhs) { return lhs.min < rhs.min; });

        // merge overlapping ranges 
        auto pos = ranges.begin();
        for (auto next = pos+1; next != ranges.end(); ++next)
        {
            if (pos->max + 1 >= next->min)
            {
                if (next->max > pos->max)
                    pos->max = next->max;
            }
            else
                *++pos = *next;
        }
        ranges.erase(pos+1, ranges.end());
    }

    AstNodePtr character_class()
    {
        const bool negative = m_pos != m_regex.end() and *m_pos == '^';
        if (negative)
            ++m_pos;

        Vector<CharRange, MemoryDomain::Regex> ranges;
        Vector<Codepoint, MemoryDomain::Regex> excluded;
        Vector<std::pair<wctype_t, bool>, MemoryDomain::Regex> ctypes;
        while (m_pos != m_regex.end() and *m_pos != ']')
        {
            auto cp = *m_pos++;
            if (cp == '-')
            {
                ranges.push_back({ '-', '-' });
                continue;
            }

            if (at_end())
                break;

            if (cp == '\\')
            {
                auto it = find_if(character_class_escapes,
                                  [cp = to_lower(*m_pos)](auto& t) { return t.cp == cp; });
                if (it != std::end(character_class_escapes))
                {
                    auto negative = is_upper(*m_pos);
                    if (it->ctype)
                        ctypes.push_back({wctype(it->ctype), not negative});
                    for (auto& c : it->additional_chars)
                    {
                        if (negative)
                            excluded.push_back((Codepoint)c);
                        else
                            ranges.push_back({(Codepoint)c, (Codepoint)c});
                    }
                    ++m_pos;
                    continue;
                }
                else // its just an escaped character
                {
                    cp = *m_pos++;
                    for (auto& control : control_escapes)
                    {
                        if (control.name == cp)
                        {
                            cp = control.value;
                            break;
                        }
                    }
                }
            }

            CharRange range = { cp, cp };
            if (*m_pos == '-')
            {
                if (++m_pos == m_regex.end())
                    break;
                if (*m_pos != ']')
                {
                    range.max = *m_pos++;
                    if (range.min > range.max)
                        parse_error("invalid range specified");
                }
                else
                {
                    ranges.push_back(range);
                    range = { '-', '-' };
                }
            }
            ranges.push_back(range);
        }
        if (at_end())
            parse_error("unclosed character class");
        ++m_pos;

        if (m_ignore_case)
        {
            for (auto& range : ranges)
            {
                range.min = to_lower(range.max);
                range.max = to_lower(range.max);
            }
            for (auto& cp : excluded)
                cp = to_lower(cp);
        }

        normalize_ranges(ranges);

        // Optimize the relatively common case of using a character class to
        // escape a character, such as [*]
        if (ctypes.empty() and excluded.empty() and not negative and
            ranges.size() == 1 and ranges.front().min == ranges.front().max)
            return new_node(ParsedRegex::Literal, ranges.front().min);

        auto matcher = [ranges = std::move(ranges),
                        ctypes = std::move(ctypes),
                        excluded = std::move(excluded),
                        negative, ignore_case = m_ignore_case] (Codepoint cp) {
            if (ignore_case)
                cp = to_lower(cp);

            auto it = std::lower_bound(ranges.begin(), ranges.end(), cp,
                                       [](auto& range, Codepoint cp)
                                       { return range.max < cp; });

            auto found = (it != ranges.end() and it->min <= cp) or
            contains_that(ctypes, [cp](auto& c) {
                return (bool)iswctype(cp, c.first) == c.second;
            }) or (not excluded.empty() and not contains(excluded, cp));
            return negative ? not found : found;
        };

        auto matcher_id = m_parsed_regex.matchers.size();
        m_parsed_regex.matchers.push_back(std::move(matcher));

        return new_node(ParsedRegex::Matcher, matcher_id);
    }

    ParsedRegex::Quantifier quantifier()
    {
        if (at_end())
            return {ParsedRegex::Quantifier::One};

        constexpr int max_repeat = 1000;
        auto read_int = [max_repeat, this](auto& pos, auto begin, auto end) {
            int res = 0;
            for (; pos != end; ++pos)
            {
                const auto cp = *pos;
                if (cp < '0' or cp > '9')
                    return pos == begin ? -1 : res;
                res = res * 10 + cp - '0';
                if (res > max_repeat)
                    parse_error(format("Explicit quantifier is too big, maximum is {}", max_repeat));
            }
            return res;
        };

        auto check_greedy = [&]() {
            if (at_end() or *m_pos != '?')
                return true;
            ++m_pos;
            return false;
        };

        switch (*m_pos)
        {
            case '*': ++m_pos; return {ParsedRegex::Quantifier::RepeatZeroOrMore, check_greedy()};
            case '+': ++m_pos; return {ParsedRegex::Quantifier::RepeatOneOrMore, check_greedy()};
            case '?': ++m_pos; return {ParsedRegex::Quantifier::Optional, check_greedy()};
            case '{':
            {
                auto it = m_pos+1;
                const int min = read_int(it, it, m_regex.end());
                int max = min;
                if (*it == ',')
                {
                    ++it;
                    max = read_int(it, it, m_regex.end());
                }
                if (*it++ != '}')
                   parse_error("expected closing bracket");
                m_pos = it;
                return {ParsedRegex::Quantifier::RepeatMinMax, check_greedy(), min, max};
            }
            default: return {ParsedRegex::Quantifier::One};
        }
    }

    AstNodePtr new_node(ParsedRegex::Op op, Codepoint value = -1,
                        ParsedRegex::Quantifier quantifier = {ParsedRegex::Quantifier::One})
    {
        return AstNodePtr{new ParsedRegex::AstNode{op, m_ignore_case, value, quantifier, {}}};
    }

    bool at_end() const { return m_pos == m_regex.end(); }

    [[gnu::noreturn]]
    void parse_error(StringView error) const
    {
        throw regex_error(format("regex parse error: {} at '{}<<<HERE>>>{}'", error,
                                 StringView{m_regex.begin(), m_pos.base()},
                                 StringView{m_pos.base(), m_regex.end()}));
    }

    void validate_lookaround(const AstNodePtr& node)
    {
        for (auto& child : node->children)
        {
            if (child->op != ParsedRegex::Literal and child->op != ParsedRegex::Matcher and
                child->op != ParsedRegex::AnyChar)
                parse_error("Lookaround can only contain literals, any chars or character classes");
            if (child->quantifier.type != ParsedRegex::Quantifier::One)
                parse_error("Quantifiers cannot be used in lookarounds");
        }
    }

    ParsedRegex m_parsed_regex;
    StringView m_regex;
    Iterator m_pos;
    bool m_ignore_case = false;

    static constexpr struct CharacterClassEscape {
        Codepoint cp;
        const char* ctype;
        StringView additional_chars;
        bool neg;
    } character_class_escapes[] = {
        { 'd', "digit", "", false },
        { 'w', "alnum", "_", false },
        { 's', "space", "", false },
        { 'h', nullptr, " \t", false },
    };

    static constexpr struct ControlEscape {
        Codepoint name;
        Codepoint value;
    } control_escapes[] = {
        { 'f', '\f' },
        { 'n', '\n' },
        { 'r', '\r' },
        { 't', '\t' },
        { 'v', '\v' }
    };
};

constexpr RegexParser::CharacterClassEscape RegexParser::character_class_escapes[];
constexpr RegexParser::ControlEscape RegexParser::control_escapes[];

struct RegexCompiler
{
    RegexCompiler(const ParsedRegex& parsed_regex, RegexCompileFlags flags, MatchDirection direction)
        : m_parsed_regex{parsed_regex}, m_flags(flags), m_forward{direction == MatchDirection::Forward}
    {
        compile_node(m_parsed_regex.ast);
        push_inst(CompiledRegex::Match);
        constexpr auto max_instructions = std::numeric_limits<uint16_t>::max();
        if (m_program.instructions.size() >= max_instructions)
            throw regex_error(format("regex compiled to more than {} instructions", max_instructions));
        m_program.matchers = m_parsed_regex.matchers;
        m_program.save_count = m_parsed_regex.capture_count * 2;
        m_program.direction = direction;
        m_program.start_chars = compute_start_chars();
    }

    CompiledRegex get_compiled_regex() { return std::move(m_program); }

private:

    uint32_t compile_node_inner(const ParsedRegex::AstNodePtr& node)
    {
        const auto start_pos = m_program.instructions.size();
        const bool ignore_case = node->ignore_case;

        const Codepoint capture = (node->op == ParsedRegex::Alternation or node->op == ParsedRegex::Sequence) ? node->value : -1;
        if (capture != -1 and (capture == 0 or not (m_flags & RegexCompileFlags::NoSubs)))
            push_inst(CompiledRegex::Save, capture * 2 + (m_forward ? 0 : 1));

        Vector<uint32_t> goto_inner_end_offsets;
        switch (node->op)
        {
            case ParsedRegex::Literal:
                if (ignore_case)
                    push_inst(CompiledRegex::Literal_IgnoreCase, to_lower(node->value));
                else
                    push_inst(CompiledRegex::Literal, node->value);
                break;
            case ParsedRegex::AnyChar:
                push_inst(CompiledRegex::AnyChar);
                break;
            case ParsedRegex::Matcher:
                push_inst(CompiledRegex::Matcher, node->value);
                break;
            case ParsedRegex::Sequence:
            {
                if (m_forward)
                    for (auto& child : node->children)
                        compile_node(child);
                else
                    for (auto& child : node->children | reverse())
                        compile_node(child);
                break;
            }
            case ParsedRegex::Alternation:
            {
                auto& children = node->children;
                kak_assert(children.size() > 1);

                const auto split_pos = m_program.instructions.size();
                for (int i = 0; i < children.size() - 1; ++i)
                    push_inst(CompiledRegex::Split_PrioritizeParent);

                for (int i = 0; i < children.size(); ++i)
                {
                    auto node = compile_node(children[i]);
                    if (i > 0)
                        m_program.instructions[split_pos + i - 1].param = node;
                    if (i < children.size() - 1)
                    {
                        auto jump = push_inst(CompiledRegex::Jump);
                        goto_inner_end_offsets.push_back(jump);
                    }
                }
                break;
            }
            case ParsedRegex::LookAhead:
                push_inst(m_forward ? (ignore_case ? CompiledRegex::LookAhead_IgnoreCase
                                                   : CompiledRegex::LookAhead)
                                    : (ignore_case ? CompiledRegex::LookBehind_IgnoreCase
                                                   : CompiledRegex::LookBehind),
                          push_lookaround(node->children, false, ignore_case));
                break;
            case ParsedRegex::NegativeLookAhead:
                push_inst(m_forward ? (ignore_case ? CompiledRegex::NegativeLookAhead_IgnoreCase
                                                   : CompiledRegex::NegativeLookAhead)
                                    : (ignore_case ? CompiledRegex::NegativeLookBehind_IgnoreCase
                                                   : CompiledRegex::NegativeLookBehind),
                          push_lookaround(node->children, false, ignore_case));
                break;
            case ParsedRegex::LookBehind:
                push_inst(m_forward ? (ignore_case ? CompiledRegex::LookBehind_IgnoreCase
                                                   : CompiledRegex::LookBehind)
                                    : (ignore_case ? CompiledRegex::LookAhead_IgnoreCase
                                                   : CompiledRegex::LookAhead),
                          push_lookaround(node->children, true, ignore_case));
                break;
            case ParsedRegex::NegativeLookBehind:
                push_inst(m_forward ? (ignore_case ? CompiledRegex::NegativeLookBehind_IgnoreCase
                                                   : CompiledRegex::NegativeLookBehind)
                                    : (ignore_case ? CompiledRegex::NegativeLookAhead_IgnoreCase
                                                   : CompiledRegex::NegativeLookAhead),
                          push_lookaround(node->children, true, ignore_case));
                break;
            case ParsedRegex::LineStart:
                push_inst(m_forward ? CompiledRegex::LineStart
                                    : CompiledRegex::LineEnd);
                break;
            case ParsedRegex::LineEnd:
                push_inst(m_forward ? CompiledRegex::LineEnd
                                    : CompiledRegex::LineStart);
                break;
            case ParsedRegex::WordBoundary:
                push_inst(CompiledRegex::WordBoundary);
                break;
            case ParsedRegex::NotWordBoundary:
                push_inst(CompiledRegex::NotWordBoundary);
                break;
            case ParsedRegex::SubjectBegin:
                push_inst(m_forward ? CompiledRegex::SubjectBegin
                                    : CompiledRegex::SubjectEnd);
                break;
            case ParsedRegex::SubjectEnd:
                push_inst(m_forward ? CompiledRegex::SubjectEnd
                                    : CompiledRegex::SubjectBegin);
                break;
            case ParsedRegex::ResetStart:
                push_inst(CompiledRegex::Save, 0);
                break;
        }

        for (auto& offset : goto_inner_end_offsets)
            m_program.instructions[offset].param = m_program.instructions.size();

        if (capture != -1 and (capture == 0 or not (m_flags & RegexCompileFlags::NoSubs)))
            push_inst(CompiledRegex::Save, capture * 2 + (m_forward ? 1 : 0));

        return start_pos;
    }

    uint32_t compile_node(const ParsedRegex::AstNodePtr& node)
    {
        uint32_t pos = m_program.instructions.size();
        Vector<uint32_t> goto_ends;

        auto& quantifier = node->quantifier;

        // TODO reverse, invert the way we write optional quantifiers ?

        if (quantifier.allows_none())
        {
            auto split_pos = push_inst(quantifier.greedy ? CompiledRegex::Split_PrioritizeParent
                                                         : CompiledRegex::Split_PrioritizeChild);
            goto_ends.push_back(split_pos);
        }

        auto inner_pos = compile_node_inner(node);
        // Write the node multiple times when we have a min count quantifier
        for (int i = 1; i < quantifier.min; ++i)
            inner_pos = compile_node_inner(node);

        if (quantifier.allows_infinite_repeat())
            push_inst(quantifier.greedy ? CompiledRegex::Split_PrioritizeChild
                                        : CompiledRegex::Split_PrioritizeParent,
                      inner_pos);

        // Write the node as an optional match for the min -> max counts
        else for (int i = std::max(1, quantifier.min); // STILL UGLY !
                  i < quantifier.max; ++i)
        {
            auto split_pos = push_inst(quantifier.greedy ? CompiledRegex::Split_PrioritizeParent
                                                         : CompiledRegex::Split_PrioritizeChild);
            goto_ends.push_back(split_pos);
            compile_node_inner(node);
        }

        for (auto offset : goto_ends)
            m_program.instructions[offset].param = m_program.instructions.size();

        return pos;
    }

    uint32_t push_inst(CompiledRegex::Op op, uint32_t param = 0)
    {
        uint32_t res = m_program.instructions.size();
        m_program.instructions.push_back({ op, false, false, param });
        return res;
    }

    uint32_t push_lookaround(ArrayView<const ParsedRegex::AstNodePtr> characters,
                             bool reversed, bool ignore_case)
    {
        uint32_t res = m_program.lookarounds.size();
        auto write_lookaround = [this, ignore_case](auto&& characters) {
            for (auto& character : characters) 
            {
                if (character->op == ParsedRegex::Literal)
                    m_program.lookarounds.push_back(ignore_case ? to_lower(character->value)
                                                                : character->value);
                else if (character->op == ParsedRegex::AnyChar)
                    m_program.lookarounds.push_back(0xF000);
                else if (character->op == ParsedRegex::Matcher)
                    m_program.lookarounds.push_back(0xF0001 + character->value);
                else
                    kak_assert(false);
            }
        };

        if (reversed)
            write_lookaround(characters | reverse());
        else
            write_lookaround(characters);

        m_program.lookarounds.push_back((Codepoint)-1);
        return res;
    }

    static constexpr size_t start_chars_count = CompiledRegex::StartChars::count;

    // Fills accepted and rejected according to which chars can start the given node,
    // returns true if the node did not consume the char, hence a following node in
    // sequence would be still relevant for the parent node start chars computation.
    bool compute_start_chars(const ParsedRegex::AstNodePtr& node,
                             bool (&accepted)[start_chars_count]) const
    {
        switch (node->op)
        {
            case ParsedRegex::Literal:
                if (node->value < start_chars_count)
                {
                    if (node->ignore_case)
                    {
                        accepted[to_lower(node->value)] = true;
                        accepted[to_upper(node->value)] = true;
                    }
                    else
                        accepted[node->value] = true;
                }
                return node->quantifier.allows_none();
            case ParsedRegex::AnyChar:
                for (auto& b : accepted)
                    b = true;
                return node->quantifier.allows_none();
            case ParsedRegex::Matcher:
                for (Codepoint c = 0; c < start_chars_count; ++c)
                    if (m_program.matchers[node->value](c))
                        accepted[c] = true;
                return node->quantifier.allows_none();
            case ParsedRegex::Sequence:
            {
                bool consumed = false;
                auto consumes = [&, this](auto& child) {
                    return not this->compute_start_chars(child, accepted);
                };
                if (m_forward)
                    consumed = contains_that(node->children, consumes);
                else
                    consumed = contains_that(node->children | reverse(), consumes);

                return not consumed or node->quantifier.allows_none();
            }
            case ParsedRegex::Alternation:
            {
                bool all_consumed = not node->quantifier.allows_none();
                for (auto& child : node->children)
                {
                    if (compute_start_chars(child, accepted))
                        all_consumed = false;
                }
                return not all_consumed;
            }
            case ParsedRegex::LineStart:
            case ParsedRegex::LineEnd:
            case ParsedRegex::WordBoundary:
            case ParsedRegex::NotWordBoundary:
            case ParsedRegex::SubjectBegin:
            case ParsedRegex::SubjectEnd:
            case ParsedRegex::ResetStart:
            case ParsedRegex::LookAhead:
            case ParsedRegex::LookBehind:
            case ParsedRegex::NegativeLookAhead:
            case ParsedRegex::NegativeLookBehind:
                return true;
        }
        return false;
    }

    [[gnu::noinline]]
    std::unique_ptr<CompiledRegex::StartChars> compute_start_chars() const
    {
        bool accepted[start_chars_count] = {};
        if (compute_start_chars(m_parsed_regex.ast, accepted))
            return nullptr;

        if (not contains(accepted, false))
            return nullptr;

        auto start_chars = std::make_unique<CompiledRegex::StartChars>();
        memcpy(start_chars->map, accepted, sizeof(bool[start_chars_count]));
        return start_chars;
    }

    CompiledRegex m_program;
    RegexCompileFlags m_flags;
    const ParsedRegex& m_parsed_regex;
    const bool m_forward;
};

void dump_regex(const CompiledRegex& program)
{
    int count = 0;
    for (auto& inst : program.instructions)
    {
        printf(" %03d     ", count++);
        switch (inst.op)
        {
            case CompiledRegex::Literal:
                printf("literal %lc\n", inst.param);
                break;
            case CompiledRegex::Literal_IgnoreCase:
                printf("literal (ignore case) %lc\n", inst.param);
                break;
            case CompiledRegex::AnyChar:
                printf("any char\n");
                break;
            case CompiledRegex::Jump:
                printf("jump %u\n", inst.param);
                break;
            case CompiledRegex::Split_PrioritizeParent:
            case CompiledRegex::Split_PrioritizeChild:
            {
                printf("split (prioritize %s) %u\n",
                       inst.op == CompiledRegex::Split_PrioritizeParent ? "parent" : "child",
                       inst.param);
                break;
            }
            case CompiledRegex::Save:
                printf("save %d\n", inst.param);
                break;
            case CompiledRegex::Matcher:
                printf("matcher %d\n", inst.param);
                break;
            case CompiledRegex::LineStart:
                printf("line start\n");
                break;
            case CompiledRegex::LineEnd:
                printf("line end\n");
                break;
            case CompiledRegex::WordBoundary:
                printf("word boundary\n");
                break;
            case CompiledRegex::NotWordBoundary:
                printf("not word boundary\n");
                break;
            case CompiledRegex::SubjectBegin:
                printf("subject begin\n");
                break;
            case CompiledRegex::SubjectEnd:
                printf("subject end\n");
                break;
            case CompiledRegex::LookAhead:
            case CompiledRegex::NegativeLookAhead:
            case CompiledRegex::LookBehind:
            case CompiledRegex::NegativeLookBehind:
            case CompiledRegex::LookAhead_IgnoreCase:
            case CompiledRegex::NegativeLookAhead_IgnoreCase:
            case CompiledRegex::LookBehind_IgnoreCase:
            case CompiledRegex::NegativeLookBehind_IgnoreCase:
            {
                const char* name = nullptr;
                if (inst.op == CompiledRegex::LookAhead)
                    name = "look ahead";
                if (inst.op == CompiledRegex::NegativeLookAhead)
                    name = "negative look ahead";
                if (inst.op == CompiledRegex::LookBehind)
                    name = "look behind";
                if (inst.op == CompiledRegex::NegativeLookBehind)
                    name = "negative look behind";

                if (inst.op == CompiledRegex::LookAhead_IgnoreCase)
                    name = "look ahead (ignore case)";
                if (inst.op == CompiledRegex::NegativeLookAhead_IgnoreCase)
                    name = "negative look ahead (ignore case)";
                if (inst.op == CompiledRegex::LookBehind_IgnoreCase)
                    name = "look behind (ignore case)";
                if (inst.op == CompiledRegex::NegativeLookBehind_IgnoreCase)
                    name = "negative look behind (ignore case)";

                String str;
                for (auto it = program.lookarounds.begin() + inst.param; *it != -1; ++it)
                    utf8::dump(std::back_inserter(str), *it);
                printf("%s (%s)\n", name, str.c_str());
                break;
            }
            case CompiledRegex::Match:
                printf("match\n");
        }
    }
}

CompiledRegex compile_regex(StringView re, RegexCompileFlags flags, MatchDirection direction)
{
    return RegexCompiler{RegexParser::parse(re), flags, direction}.get_compiled_regex();
}

namespace
{
template<MatchDirection dir = MatchDirection::Forward>
struct TestVM : CompiledRegex, ThreadedRegexVM<const char*, dir>
{
    using VMType = ThreadedRegexVM<const char*, dir>;

    TestVM(StringView re, bool dump = false)
        : CompiledRegex{compile_regex(re, RegexCompileFlags::None, dir)},
          VMType{(const CompiledRegex&)*this}
    { if (dump) dump_regex(*this); }

    bool exec(StringView re, RegexExecFlags flags = RegexExecFlags::AnyMatch)
    {
        return VMType::exec(re.begin(), re.end(), flags);
    }
};
}

auto test_regex = UnitTest{[]{
    {
        TestVM<> vm{R"(a*b)"};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("acb"));
        kak_assert(not vm.exec("abc"));
        kak_assert(not vm.exec(""));
    }

    {
        TestVM<> vm{R"(^a.*b$)"};
        kak_assert(vm.exec("afoob"));
        kak_assert(vm.exec("ab"));
        kak_assert(not vm.exec("bab"));
        kak_assert(not vm.exec(""));
    }

    {
        TestVM<> vm{R"(^(foo|qux|baz)+(bar)?baz$)"};
        kak_assert(vm.exec("fooquxbarbaz"));
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "qux");
        kak_assert(not vm.exec("fooquxbarbaze"));
        kak_assert(not vm.exec("quxbar"));
        kak_assert(not vm.exec("blahblah"));
        kak_assert(vm.exec("bazbaz"));
        kak_assert(vm.exec("quxbaz"));
    }

    {
        TestVM<> vm{R"(.*\b(foo|bar)\b.*)"};
        kak_assert(vm.exec("qux foo baz"));
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "foo");
        kak_assert(not vm.exec("quxfoobaz"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }

    {
        TestVM<> vm{R"((foo|bar))"};
        kak_assert(vm.exec("foo"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }

    {
        TestVM<> vm{R"(a{3,5}b)"};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaaaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        TestVM<> vm{R"(a{3}b)"};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaab"));
    }

    {
        TestVM<> vm{R"(a{3,}b)"};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        TestVM<> vm{R"(a{,3}b)"};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaab"));
    }

    {
        TestVM<> vm{R"(f.*a(.*o))"};
        kak_assert(vm.exec("blahfoobarfoobaz", RegexExecFlags::Search));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]} == "foobarfoo");
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "rfoo");
        kak_assert(vm.exec("mais que fais la police", RegexExecFlags::Search));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]} == "fais la po");
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == " po");
    }

    {
        TestVM<> vm{R"([àb-dX-Z-]{3,5})"};
        kak_assert(vm.exec("cà-Y"));
        kak_assert(not vm.exec("àeY"));
        kak_assert(vm.exec("dcbàX"));
        kak_assert(not vm.exec("efg"));
    }

    {
        TestVM<> vm{R"((a{3,5})a+)"};
        kak_assert(vm.exec("aaaaaa", RegexExecFlags::None));
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "aaaaa");
    }

    {
        TestVM<> vm{R"((a{3,5}?)a+)"};
        kak_assert(vm.exec("aaaaaa", RegexExecFlags::None));
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "aaa");
    }

    {
        TestVM<> vm{R"((a{3,5}?)a)"};
        kak_assert(vm.exec("aaaa"));
    }

    {
        TestVM<> vm{R"(\d{3})"};
        kak_assert(vm.exec("123"));
        kak_assert(not vm.exec("1x3"));
    }

    {
        TestVM<> vm{R"([-\d]+)"};
        kak_assert(vm.exec("123-456"));
        kak_assert(not vm.exec("123_456"));
    }

    {
        TestVM<> vm{R"([ \H]+)"};
        kak_assert(vm.exec("abc "));
        kak_assert(not vm.exec("a \t"));
    }

    {
        TestVM<> vm{R"(\Q{}[]*+?\Ea+)"};
        kak_assert(vm.exec("{}[]*+?aa"));
    }

    {
        TestVM<> vm{R"(\Q...)"};
        kak_assert(vm.exec("..."));
        kak_assert(not vm.exec("bla"));
    }

    {
        TestVM<> vm{R"(foo\Kbar)"};
        kak_assert(vm.exec("foobar", RegexExecFlags::None));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]} == "bar");
        kak_assert(not vm.exec("bar", RegexExecFlags::None));
    }

    {
        TestVM<> vm{R"((fo+?).*)"};
        kak_assert(vm.exec("foooo", RegexExecFlags::None));
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]} == "fo");
    }

    {
        TestVM<> vm{R"((?=foo).)"};
        kak_assert(vm.exec("barfoo", RegexExecFlags::Search));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]} == "f");
    }

    {
        TestVM<> vm{R"((?<!f).)"};
        kak_assert(vm.exec("f"));
    }

    {
        TestVM<> vm{R"((?!f[oa]o)...)"};
        kak_assert(not vm.exec("foo"));
        kak_assert(vm.exec("qux"));
    }

    {
        TestVM<> vm{R"(...(?<=f.o))"};
        kak_assert(vm.exec("foo"));
        kak_assert(not vm.exec("qux"));
    }

    {
        TestVM<> vm{R"(...(?<!foo))"};
        kak_assert(not vm.exec("foo"));
        kak_assert(vm.exec("qux"));
    }

    {
        TestVM<> vm{R"(Foo(?i)f[oB]+)"};
        kak_assert(vm.exec("FooFOoBb"));
    }

    {
        TestVM<> vm{R"([^\]]+)"};
        kak_assert(not vm.exec("a]c"));
        kak_assert(vm.exec("abc"));
    }

    {
        TestVM<> vm{R"([^:\n]+)"};
        kak_assert(not vm.exec("\nbc"));
        kak_assert(vm.exec("abc"));
    }

    {
        TestVM<> vm{R"((?:foo)+)"};
        kak_assert(vm.exec("foofoofoo"));
        kak_assert(not vm.exec("barbarbar"));
    }

    {
        TestVM<> vm{R"((?<!\\)(?:\\\\)*")"};
        kak_assert(vm.exec("foo\"", RegexExecFlags::Search));
    }

    {
        TestVM<> vm{R"($)"};
        kak_assert(vm.exec("foo\n", RegexExecFlags::Search));
        kak_assert(*vm.captures()[0] == '\n');
    }

    {
        TestVM<MatchDirection::Backward> vm{R"(fo{1,})"};
        kak_assert(vm.exec("foo1fooo2", RegexExecFlags::Search));
        kak_assert(*vm.captures()[1] == '2');
    }

    {
        TestVM<MatchDirection::Backward> vm{R"((?<=f)oo(b[ae]r)?(?=baz))"};
        kak_assert(vm.exec("foobarbazfoobazfooberbaz", RegexExecFlags::Search));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]}  == "oober");
        kak_assert(StringView{vm.captures()[2], vm.captures()[3]}  == "ber");
    }

    {
        TestVM<MatchDirection::Backward> vm{R"((baz|boz|foo|qux)(?<!baz)(?<!o))"};
        kak_assert(vm.exec("quxbozfoobaz", RegexExecFlags::Search));
        kak_assert(StringView{vm.captures()[0], vm.captures()[1]}  == "boz");
    }

    {
        TestVM<MatchDirection::Backward> vm{R"(foo)"};
        kak_assert(vm.exec("foofoo", RegexExecFlags::Search));
        kak_assert(*vm.captures()[1]  == 0);
    }

    {
        TestVM<MatchDirection::Backward> vm{R"($)"};
        kak_assert(vm.exec("foo\nbar\nbaz\nqux", RegexExecFlags::Search | RegexExecFlags::NotEndOfLine));
        kak_assert(StringView{vm.captures()[0]}  == "\nqux");
    }

    {
        TestVM<> vm{R"(()*)"};
        kak_assert(not vm.exec(" "));
    }

    {
        TestVM<> vm{R"(\b(?<!-)(a|b|)(?!-)\b)"};
        kak_assert(vm.exec("# foo bar", RegexExecFlags::Search));
        kak_assert(*vm.captures()[0] == '#');
    }

    {
        TestVM<> vm{R"((?=))"};
        kak_assert(vm.exec(""));
    }

    {
        TestVM<> vm{R"((?i)FOO)"};
        kak_assert(vm.exec("foo", RegexExecFlags::Search));
    }

    {
        TestVM<> vm{R"((?i)(?=Foo))"};
        kak_assert(vm.exec("fOO", RegexExecFlags::Search));
        kak_assert(*vm.captures()[0] == 'f');
    }

    {
        TestVM<> vm{R"([d-ea-dcf-k]+)"};
        kak_assert(vm.exec("abcde"));
    }
}};

}
