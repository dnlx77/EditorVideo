#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace editor::core {

// Un valore JSON, quanto basta per i file di progetto: i numeri interi restano interi esatti (64 bit), le stringhe
// sono UTF-8 e gli oggetti tengono le chiavi in ordine alfabetico.
class JsonValue
{
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue() = default;  // null
    JsonValue(std::nullptr_t) {}
    JsonValue(bool value) : value_(value) {}
    JsonValue(int value) : value_(static_cast<std::int64_t>(value)) {}
    JsonValue(std::int64_t value) : value_(value) {}
    JsonValue(double value) : value_(value) {}
    JsonValue(const char* value) : value_(std::string(value)) {}
    JsonValue(std::string value) : value_(std::move(value)) {}
    JsonValue(Array value) : value_(std::make_shared<Array>(std::move(value))) {}
    JsonValue(Object value) : value_(std::make_shared<Object>(std::move(value))) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(value_); }
    bool isBool() const { return std::holds_alternative<bool>(value_); }
    bool isInteger() const { return std::holds_alternative<std::int64_t>(value_); }
    bool isNumber() const { return isInteger() || std::holds_alternative<double>(value_); }
    bool isString() const { return std::holds_alternative<std::string>(value_); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(value_); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(value_); }

    // Lanciano JsonError se il tipo non è quello giusto.
    bool asBool() const;
    std::int64_t asInteger() const;
    double asNumber() const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;

    // Oggetto: il valore della chiave, null se non c'è (o se questo non è un oggetto).
    const JsonValue& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;

    // Testo JSON indentato (2 spazi); gli array di soli numeri restano su una riga.
    std::string dump() const;

private:
    void write(std::string& out, int indent) const;

    std::variant<std::monostate, bool, std::int64_t, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>>
        value_;
};

// Errore di lettura o di tipo, con un messaggio in italiano (es. "riga 3, colonna 7: atteso ':'").
class JsonError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Legge un testo JSON (UTF-8). Lancia JsonError se non è valido.
JsonValue parseJson(const std::string& text);

}  // namespace editor::core
