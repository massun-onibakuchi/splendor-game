#include "splendor/catalog.hpp"

#include "splendor/contracts.hpp"
#include "picosha2/picosha2.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace splendor {
namespace {

class JsonCursor {
public:
    explicit JsonCursor(std::string_view input) : input_(input) {}

    void whitespace() {
        while (position_ < input_.size()
               && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char expected) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail(std::string("expected '") + expected + "'");
    }

    std::string string() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '"') fail("expected string");
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char ch = input_[position_++];
            if (ch == '"') return result;
            if (ch == '\\') {
                if (position_ >= input_.size()) fail("truncated string escape");
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    default: fail("unsupported catalog string escape");
                }
                continue;
            }
            if (ch < 0x20) fail("control character in string");
            result.push_back(static_cast<char>(ch));
        }
        fail("unterminated string");
    }

    std::uint64_t unsigned_integer() {
        whitespace();
        if (position_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            fail("expected unsigned integer");
        }
        if (input_[position_] == '0' && position_ + 1 < input_.size()
            && std::isdigit(static_cast<unsigned char>(input_[position_ + 1]))) {
            fail("leading zero in integer");
        }
        std::uint64_t result = 0;
        while (position_ < input_.size()
               && std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
            const unsigned digit = static_cast<unsigned>(input_[position_++] - '0');
            if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                fail("integer overflow");
            }
            result = result * 10U + digit;
        }
        return result;
    }

    void finish() {
        whitespace();
        if (position_ != input_.size()) fail("trailing JSON content");
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(
            "catalog JSON at byte " + std::to_string(position_) + ": " + message);
    }

private:
    std::string_view input_;
    std::size_t position_{};
};

template <typename T>
T narrow(JsonCursor& cursor, const std::uint64_t value, const char* field) {
    if (value > std::numeric_limits<T>::max()) cursor.fail(std::string(field) + " is out of range");
    return static_cast<T>(value);
}

Color parse_color(JsonCursor& cursor) {
    const std::string value = cursor.string();
    if (value == "white") return Color::White;
    if (value == "blue") return Color::Blue;
    if (value == "green") return Color::Green;
    if (value == "red") return Color::Red;
    if (value == "black") return Color::Black;
    cursor.fail("unknown color");
}

std::array<std::uint8_t, kGemColorCount> parse_gems(JsonCursor& cursor) {
    std::array<std::uint8_t, kGemColorCount> values{};
    cursor.expect('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) cursor.expect(',');
        values[index] = narrow<std::uint8_t>(cursor, cursor.unsigned_integer(), "gem value");
    }
    cursor.expect(']');
    return values;
}

std::vector<Card> parse_cards(JsonCursor& cursor) {
    std::vector<Card> cards;
    cursor.expect('[');
    if (cursor.consume(']')) return cards;
    while (true) {
        cursor.expect('[');
        Card card;
        card.id = narrow<std::uint16_t>(cursor, cursor.unsigned_integer(), "card id");
        cursor.expect(',');
        card.tier = narrow<std::uint8_t>(cursor, cursor.unsigned_integer(), "card tier");
        cursor.expect(',');
        card.bonus = parse_color(cursor);
        cursor.expect(',');
        card.prestige = narrow<std::uint8_t>(cursor, cursor.unsigned_integer(), "card prestige");
        cursor.expect(',');
        card.cost = parse_gems(cursor);
        cursor.expect(']');
        cards.push_back(card);
        if (cursor.consume(']')) return cards;
        cursor.expect(',');
    }
}

std::vector<Noble> parse_nobles(JsonCursor& cursor) {
    std::vector<Noble> nobles;
    cursor.expect('[');
    if (cursor.consume(']')) return nobles;
    while (true) {
        cursor.expect('[');
        Noble noble;
        noble.id = narrow<std::uint16_t>(cursor, cursor.unsigned_integer(), "noble id");
        cursor.expect(',');
        noble.prestige = narrow<std::uint8_t>(cursor, cursor.unsigned_integer(), "noble prestige");
        cursor.expect(',');
        noble.requirements = parse_gems(cursor);
        cursor.expect(']');
        nobles.push_back(noble);
        if (cursor.consume(']')) return nobles;
        cursor.expect(',');
    }
}

void parse_color_order(JsonCursor& cursor) {
    constexpr std::array expected{"white", "blue", "green", "red", "black"};
    cursor.expect('[');
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (index != 0) cursor.expect(',');
        if (cursor.string() != expected[index]) cursor.fail("invalid color order");
    }
    cursor.expect(']');
}

std::string_view color_name(const Color color) {
    switch (color) {
        case Color::White: return "white";
        case Color::Blue: return "blue";
        case Color::Green: return "green";
        case Color::Red: return "red";
        case Color::Black: return "black";
        case Color::Gold: return "gold";
    }
    throw std::logic_error("invalid color");
}

std::string canonical_catalog(
    const std::string_view contract,
    const std::array<Card, kCardCount>& cards,
    const std::array<Noble, kNobleCount>& nobles) {
    std::string out = "{\"cards\":[";
    for (std::size_t index = 0; index < cards.size(); ++index) {
        if (index != 0) out.push_back(',');
        const Card& card = cards[index];
        out += "[" + std::to_string(card.id) + "," + std::to_string(card.tier) + ",\"";
        out += color_name(card.bonus);
        out += "\"," + std::to_string(card.prestige) + ",[";
        for (std::size_t color = 0; color < card.cost.size(); ++color) {
            if (color != 0) out.push_back(',');
            out += std::to_string(card.cost[color]);
        }
        out += "]]";
    }
    out += "],\"catalog_contract\":\"";
    out += contract;
    out += "\",\"color_order\":[\"white\",\"blue\",\"green\",\"red\",\"black\"],\"nobles\":[";
    for (std::size_t index = 0; index < nobles.size(); ++index) {
        if (index != 0) out.push_back(',');
        const Noble& noble = nobles[index];
        out += "[" + std::to_string(noble.id) + "," + std::to_string(noble.prestige) + ",[";
        for (std::size_t color = 0; color < noble.requirements.size(); ++color) {
            if (color != 0) out.push_back(',');
            out += std::to_string(noble.requirements[color]);
        }
        out += "]]";
    }
    out += "]}";
    return out;
}

}  // namespace

Catalog Catalog::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("failed to open catalog: " + path.string());
    const std::string bytes{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    JsonCursor cursor(bytes);
    cursor.expect('{');

    std::vector<Card> cards;
    std::vector<Noble> nobles;
    std::string contract;
    bool have_cards = false;
    bool have_contract = false;
    bool have_colors = false;
    bool have_nobles = false;
    if (!cursor.consume('}')) {
        while (true) {
            const std::string key = cursor.string();
            cursor.expect(':');
            if (key == "cards" && !have_cards) {
                cards = parse_cards(cursor);
                have_cards = true;
            } else if (key == "catalog_contract" && !have_contract) {
                contract = cursor.string();
                // The canonical payload embeds the name unescaped.
                if (contract.empty() || !std::all_of(contract.begin(), contract.end(), [](const char ch) {
                        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
                    })) {
                    cursor.fail("catalog contract must be nonempty [a-z0-9.-]");
                }
                have_contract = true;
            } else if (key == "color_order" && !have_colors) {
                parse_color_order(cursor);
                have_colors = true;
            } else if (key == "nobles" && !have_nobles) {
                nobles = parse_nobles(cursor);
                have_nobles = true;
            } else {
                cursor.fail("unknown or duplicate catalog field: " + key);
            }
            if (cursor.consume('}')) break;
            cursor.expect(',');
        }
    }
    cursor.finish();
    if (!(have_cards && have_contract && have_colors && have_nobles)) {
        throw std::runtime_error("catalog is missing a required field");
    }
    if (cards.size() != kCardCount || nobles.size() != kNobleCount) {
        throw std::runtime_error("catalog row count does not match its contract");
    }

    Catalog result;
    for (std::size_t index = 0; index < cards.size(); ++index) {
        const Card& card = cards[index];
        if (card.id != index || card.tier < 1 || card.tier > 3 || card.bonus == Color::Gold) {
            throw std::runtime_error("invalid card identity or category");
        }
        result.cards_[index] = card;
        result.tiers_[card.tier - 1].insert(card.id);
    }
    for (std::size_t index = 0; index < nobles.size(); ++index) {
        if (nobles[index].id != index) throw std::runtime_error("invalid noble identity");
        result.nobles_[index] = nobles[index];
    }
    result.payload_sha256_ =
        picosha2::hash256_hex_string(canonical_catalog(contract, result.cards_, result.nobles_));
    result.contract_ = std::move(contract);
    return result;
}

void Catalog::throw_unknown_card() { throw std::out_of_range("unknown card id"); }

void Catalog::throw_unknown_noble() { throw std::out_of_range("unknown noble id"); }

const CardSet& Catalog::card_ids_for_tier(const std::uint8_t tier) const {
    if (tier < 1 || tier > 3) throw std::invalid_argument("tier must be 1, 2, or 3");
    return tiers_[tier - 1];
}

}  // namespace splendor
