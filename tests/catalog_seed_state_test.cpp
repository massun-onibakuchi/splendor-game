#include "splendor/contracts.hpp"
#include "splendor/seed.hpp"
#include "splendor/catalog.hpp"
#include "splendor/state.hpp"
#include "picosha2/picosha2.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace splendor;

// The seed law by its definition: the first eight bytes, big-endian, of SHA-256 over the
// RFC 8785 object {contract, coordinates (decimal strings), domain, root (decimal string)}.
std::uint64_t reference_derive64(
    const std::uint64_t root, const std::string& domain, const std::vector<std::uint64_t>& coordinates) {
    std::string preimage = "{\"contract\":\"" + std::string(kSeedContract) + "\",\"coordinates\":[";
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        if (index) preimage += ',';
        preimage += '"' + std::to_string(coordinates[index]) + '"';
    }
    preimage += "],\"domain\":\"" + domain + "\",\"root\":\"" + std::to_string(root) + "\"}";
    std::array<unsigned char, picosha2::k_digest_size> digest{};
    picosha2::hash256(preimage.begin(), preimage.end(), digest.begin(), digest.end());
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) value = (value << 8) | digest[byte];
    return value;
}

TEST(CatalogSeedStateTest, TheSeedLawIsItsDefinitionForEveryShapeOfInput) {
    for (const auto& [root, domain, coordinates] : std::vector<std::tuple<std::uint64_t, std::string, std::vector<std::uint64_t>>>{
             {0, "setup", {0}},
             {UINT64_MAX, "x", {}},
             {7, "draw", {UINT64_MAX, 3, 0}},
             {123456789, std::string(90, 'd'), {1, 2, 3, 4, 5, 6, 7, 8}}}) {
        // Twice: the second derivation reads the cached first block.
        for (int pass = 0; pass < 2; ++pass) {
            EXPECT_EQ(derive64(root, domain, coordinates), reference_derive64(root, domain, coordinates))
                << domain;
        }
    }
    EXPECT_GT(u01(0), 0.0);
    EXPECT_LT(u01(UINT64_MAX), 1.0);
}

TEST(CatalogSeedStateTest, ACatalogIsIdentifiedByItsDeclaredContractAndPayload) {
    const Catalog catalog = Catalog::load(SPLENDOR_GAME_CATALOG);
    EXPECT_EQ(catalog.contract(), kCatalogContract);
    EXPECT_EQ(catalog.payload_sha256(), kCatalogPayloadSha256);
    EXPECT_EQ(catalog.card(0).bonus, Color::Black);
    EXPECT_EQ(catalog.card_ids_for_tier(1).size(), 40U);
    EXPECT_EQ(catalog.card_ids_for_tier(2).size(), 30U);
    EXPECT_EQ(catalog.card_ids_for_tier(3).size(), 20U);

    // The same cards under another declared name are another catalog.
    std::ifstream source(SPLENDOR_GAME_CATALOG, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
    const std::string standard = "\"catalog_contract\":\"" + std::string(kCatalogContract) + "\"";
    ASSERT_NE(bytes.find(standard), std::string::npos);
    bytes.replace(bytes.find(standard), standard.size(), "\"catalog_contract\":\"house-catalog-v1\"");
    const std::filesystem::path renamed =
        std::filesystem::temp_directory_path() / "splendor-game-house-catalog.json";
    std::ofstream(renamed, std::ios::binary) << bytes;
    const Catalog house = Catalog::load(renamed);
    EXPECT_EQ(house.contract(), "house-catalog-v1");
    EXPECT_NE(house.payload_sha256(), catalog.payload_sha256());
    EXPECT_EQ(house.cards(), catalog.cards());
    EXPECT_EQ(house.nobles(), catalog.nobles());

    bytes.replace(bytes.find("house-catalog-v1"), 16, "House Catalog v1");
    std::ofstream(renamed, std::ios::binary) << bytes;
    EXPECT_THROW(Catalog::load(renamed), std::runtime_error);
    std::filesystem::remove(renamed);
}

TEST(CatalogSeedStateTest, InitialSetupIsDeterministicAndValid) {
    const Catalog catalog = Catalog::load(SPLENDOR_GAME_CATALOG);
    constexpr std::uint32_t max_game_plies = 180;
    const State first = initial_state(catalog, max_game_plies, 1234);
    EXPECT_EQ(first, initial_state(catalog, max_game_plies, 1234));
    EXPECT_EQ(first.unknown_card_ids.size(), 78U);
    EXPECT_TRUE(validate_state(first, max_game_plies, catalog).valid);

    State invalid = first;
    invalid.unknown_card_ids[1] = invalid.unknown_card_ids[0];
    EXPECT_FALSE(validate_state(invalid, max_game_plies, catalog).valid);
}

}  // namespace
