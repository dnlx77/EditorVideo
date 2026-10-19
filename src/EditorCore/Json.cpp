#include "EditorCore/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace editor::core {

namespace {

const JsonValue kNull;

void writeString(std::string& out, const std::string& text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out += c;  // UTF-8 così com'è
            }
        }
    }
    out += '"';
}

class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    JsonValue parseDocument()
    {
        skipSpace();
        JsonValue value = parseValue(0);
        skipSpace();
        if (pos_ != text_.size())
            fail("testo in più dopo la fine");
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        int line = 1;
        int column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw JsonError("riga " + std::to_string(line) + ", colonna " + std::to_string(column) + ": " + what);
    }

    void skipSpace()
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(const char* word)
    {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(pos_, length, word) != 0)
            return false;
        pos_ += length;
        return true;
    }

    void expect(char c)
    {
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_] != c)
            fail(std::string("atteso '") + c + "'");
        ++pos_;
    }

    JsonValue parseValue(int depth)
    {
        if (depth > 200)
            fail("troppi livelli annidati");
        skipSpace();
        if (pos_ >= text_.size())
            fail("il testo finisce prima del previsto");
        const char c = text_[pos_];
        if (c == '{')
            return parseObject(depth);
        if (c == '[')
            return parseArray(depth);
        if (c == '"')
            return parseString();
        if (consume("true"))
            return true;
        if (consume("false"))
            return false;
        if (consume("null"))
            return nullptr;
        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber();
        fail(std::string("carattere inatteso '") + c + "'");
    }

    JsonValue parseObject(int depth)
    {
        ++pos_;  // {
        JsonValue::Object object;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return object;
        }
        for (;;) {
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                fail("attesa una chiave fra virgolette");
            std::string key = parseString().asString();
            expect(':');
            object[std::move(key)] = parseValue(depth + 1);
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            expect('}');
            return object;
        }
    }

    JsonValue parseArray(int depth)
    {
        ++pos_;  // [
        JsonValue::Array array;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return array;
        }
        for (;;) {
            array.push_back(parseValue(depth + 1));
            skipSpace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            expect(']');
            return array;
        }
    }

    unsigned parseHex4()
    {
        if (pos_ + 4 > text_.size())
            fail("sequenza \\u incompleta");
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, value, 16);
        if (ec != std::errc() || end != text_.data() + pos_ + 4)
            fail("sequenza \\u non valida");
        pos_ += 4;
        return value;
    }

    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    JsonValue parseString()
    {
        ++pos_;  // "
        std::string out;
        for (;;) {
            if (pos_ >= text_.size())
                fail("stringa senza virgolette di chiusura");
            const char c = text_[pos_++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                fail("carattere di controllo dentro una stringa");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size())
                fail("stringa senza virgolette di chiusura");
            const char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = parseHex4();
                if (cp >= 0xD800 && cp < 0xDC00) {
                    // Coppia surrogata (caratteri oltre U+FFFF)
                    if (!consume("\\u"))
                        fail("coppia surrogata incompleta");
                    const unsigned low = parseHex4();
                    if (low < 0xDC00 || low >= 0xE000)
                        fail("coppia surrogata non valida");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                fail(std::string("sequenza di escape non valida \\") + e);
            }
        }
    }

    JsonValue parseNumber()
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-')
            ++pos_;
        bool integer = true;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                integer = false;
                ++pos_;
            } else {
                break;
            }
        }
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        if (integer) {
            std::int64_t value = 0;
            const auto [end, ec] = std::from_chars(first, last, value);
            if (ec == std::errc() && end == last)
                return value;
        }
        double value = 0.0;
        const auto [end, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || end != last || !std::isfinite(value)) {
            pos_ = start;
            fail("numero non valido");
        }
        return value;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

}  // namespace

bool JsonValue::asBool() const
{
    if (!isBool())
        throw JsonError("atteso vero o falso");
    return std::get<bool>(value_);
}

std::int64_t JsonValue::asInteger() const
{
    if (isInteger())
        return std::get<std::int64_t>(value_);
    if (std::holds_alternative<double>(value_)) {
        const double d = std::get<double>(value_);
        if (std::floor(d) == d && std::abs(d) < 9.0e18)
            return static_cast<std::int64_t>(d);
    }
    throw JsonError("atteso un numero intero");
}

double JsonValue::asNumber() const
{
    if (isInteger())
        return static_cast<double>(std::get<std::int64_t>(value_));
    if (std::holds_alternative<double>(value_))
        return std::get<double>(value_);
    throw JsonError("atteso un numero");
}

const std::string& JsonValue::asString() const
{
    if (!isString())
        throw JsonError("attesa una stringa");
    return std::get<std::string>(value_);
}

const JsonValue::Array& JsonValue::asArray() const
{
    if (!isArray())
        throw JsonError("atteso un elenco");
    return *std::get<std::shared_ptr<Array>>(value_);
}

const JsonValue::Object& JsonValue::asObject() const
{
    if (!isObject())
        throw JsonError("atteso un oggetto");
    return *std::get<std::shared_ptr<Object>>(value_);
}

const JsonValue& JsonValue::operator[](const std::string& key) const
{
    if (!isObject())
        return kNull;
    const Object& object = asObject();
    const auto it = object.find(key);
    return it != object.end() ? it->second : kNull;
}

bool JsonValue::contains(const std::string& key) const
{
    return isObject() && asObject().contains(key);
}

std::string JsonValue::dump() const
{
    std::string out;
    write(out, 0);
    out += '\n';
    return out;
}

void JsonValue::write(std::string& out, int indent) const
{
    const std::string pad(static_cast<std::size_t>(indent + 2), ' ');
    const std::string closePad(static_cast<std::size_t>(indent), ' ');
    if (isNull()) {
        out += "null";
    } else if (isBool()) {
        out += asBool() ? "true" : "false";
    } else if (isInteger()) {
        out += std::to_string(asInteger());
    } else if (std::holds_alternative<double>(value_)) {
        char buffer[64];
        const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(value_));
        out.append(buffer, ec == std::errc() ? end : buffer);
    } else if (isString()) {
        writeString(out, asString());
    } else if (isArray()) {
        const Array& array = asArray();
        bool flat = true;
        for (const JsonValue& v : array)
            flat = flat && !v.isArray() && !v.isObject();
        if (array.empty() || flat) {
            out += '[';
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i > 0)
                    out += ", ";
                array[i].write(out, indent);
            }
            out += ']';
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < array.size(); ++i) {
            out += pad;
            array[i].write(out, indent + 2);
            out += i + 1 < array.size() ? ",\n" : "\n";
        }
        out += closePad + ']';
    } else {
        const Object& object = asObject();
        if (object.empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        std::size_t i = 0;
        for (const auto& [key, value] : object) {
            out += pad;
            writeString(out, key);
            out += ": ";
            value.write(out, indent + 2);
            out += ++i < object.size() ? ",\n" : "\n";
        }
        out += closePad + '}';
    }
}

JsonValue parseJson(const std::string& text)
{
    return Parser(text).parseDocument();
}

}  // namespace editor::core
