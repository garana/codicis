/**
 * @file test_config.cc
 * @brief Unit tests for the codicis_config subsystem.
 */

#include "catch_amalgamated.hpp"

#include "codicis/config/config.h"
#include "codicis/config/option.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace codicis;

namespace {

/**
 * @brief Build a registry with a representative set of options for tests.
 * @return A populated OptionRegistry.
 */
OptionRegistry MakeRegistry() {
  OptionRegistry reg;
  reg.add_int_range("net.http_port", 8080, 1, 65535, "HTTP port");
  reg.add_string("net.bind_address", "127.0.0.1", "Bind address");
  reg.add_int("book.mem_levels", 100, "In-memory levels");
  reg.add_bool("feature.enabled", false, "A boolean feature");
  return reg;
}

/**
 * @brief Write @p contents to a uniquely named temp file.
 * @param tag      A test-unique tag used in the filename.
 * @param contents The file body.
 * @return The absolute path to the written file.
 */
std::string WriteTemp(const std::string& tag, const std::string& contents) {
  std::filesystem::path p =
      std::filesystem::temp_directory_path() / ("codicis_test_" + tag + ".conf");
  std::ofstream out{p, std::ios::binary};
  out << contents;
  out.close();
  return p.string();
}

/**
 * @brief Invoke Config::load with a vector of CLI tokens (argv[0] synthesized).
 * @param reg    The registry.
 * @param tokens CLI tokens after the program name.
 * @return The load result.
 */
Result<Config> LoadCli(const OptionRegistry& reg,
                       const std::vector<std::string>& tokens) {
  std::vector<const char*> argv;
  argv.push_back("codicis");
  for (const std::string& t : tokens) {
    argv.push_back(t.c_str());
  }
  return Config::load(reg, static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("Defaults apply when nothing overrides", "[config]") {
  OptionRegistry reg = MakeRegistry();
  Result<Config> r = LoadCli(reg, {});
  REQUIRE(r.ok());
  REQUIRE(r.value().get_int("net.http_port").value() == 8080);
  REQUIRE(r.value().get_string("net.bind_address").value() == "127.0.0.1");
  REQUIRE(r.value().get_bool("feature.enabled").value() == false);
}

TEST_CASE("CLI overrides in both --k=v and --k v forms", "[config]") {
  OptionRegistry reg = MakeRegistry();

  SECTION("inline form") {
    Result<Config> r = LoadCli(reg, {"--net.http_port=9090"});
    REQUIRE(r.ok());
    REQUIRE(r.value().get_int("net.http_port").value() == 9090);
  }
  SECTION("separated form") {
    Result<Config> r = LoadCli(reg, {"--net.http_port", "9091"});
    REQUIRE(r.ok());
    REQUIRE(r.value().get_int("net.http_port").value() == 9091);
  }
  SECTION("bare boolean flag") {
    Result<Config> r = LoadCli(reg, {"--feature.enabled"});
    REQUIRE(r.ok());
    REQUIRE(r.value().get_bool("feature.enabled").value() == true);
  }
}

TEST_CASE("File values override defaults", "[config]") {
  OptionRegistry reg = MakeRegistry();
  const std::string path = WriteTemp("file_over_default",
                                     "# a comment\n"
                                     "net.http_port = 7000\n"
                                     "net.bind_address = 0.0.0.0\n");
  std::vector<const char*> argv = {"codicis"};
  Result<Config> r = Config::load_with_file(
      reg, path, static_cast<int>(argv.size()), argv.data());
  REQUIRE(r.ok());
  REQUIRE(r.value().get_int("net.http_port").value() == 7000);
  REQUIRE(r.value().get_string("net.bind_address").value() == "0.0.0.0");
  REQUIRE(r.value().get_int("book.mem_levels").value() == 100);  // default
}

TEST_CASE("CLI overrides file (precedence)", "[config]") {
  OptionRegistry reg = MakeRegistry();
  const std::string path =
      WriteTemp("cli_over_file", "net.http_port = 7000\n");
  std::vector<const char*> argv = {"codicis", "--net.http_port=9090"};
  Result<Config> r = Config::load_with_file(
      reg, path, static_cast<int>(argv.size()), argv.data());
  REQUIRE(r.ok());
  REQUIRE(r.value().get_int("net.http_port").value() == 9090);
}

TEST_CASE("--config redirects the file path", "[config]") {
  OptionRegistry reg = MakeRegistry();
  const std::string path =
      WriteTemp("config_redirect", "book.mem_levels = 250\n");
  Result<Config> r = LoadCli(reg, {"--config", path});
  REQUIRE(r.ok());
  REQUIRE(r.value().get_int("book.mem_levels").value() == 250);
}

TEST_CASE("Unknown keys are rejected", "[config]") {
  OptionRegistry reg = MakeRegistry();

  SECTION("unknown CLI flag") {
    Result<Config> r = LoadCli(reg, {"--net.nope=1"});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == ErrorCode::kInvalidArg);
  }
  SECTION("unknown file key") {
    const std::string path = WriteTemp("unknown_key", "net.nope = 1\n");
    std::vector<const char*> argv = {"codicis"};
    Result<Config> r = Config::load_with_file(
        reg, path, static_cast<int>(argv.size()), argv.data());
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == ErrorCode::kInvalidArg);
  }
}

TEST_CASE("Type and range validation", "[config]") {
  OptionRegistry reg = MakeRegistry();

  SECTION("non-integer value") {
    Result<Config> r = LoadCli(reg, {"--net.http_port=abc"});
    REQUIRE_FALSE(r.ok());
  }
  SECTION("below minimum") {
    Result<Config> r = LoadCli(reg, {"--net.http_port=0"});
    REQUIRE_FALSE(r.ok());
  }
  SECTION("above maximum") {
    Result<Config> r = LoadCli(reg, {"--net.http_port=70000"});
    REQUIRE_FALSE(r.ok());
  }
}

TEST_CASE("Malformed flags are rejected", "[config]") {
  OptionRegistry reg = MakeRegistry();

  SECTION("missing value for non-bool") {
    Result<Config> r = LoadCli(reg, {"--net.http_port"});
    REQUIRE_FALSE(r.ok());
  }
  SECTION("positional argument") {
    Result<Config> r = LoadCli(reg, {"stray"});
    REQUIRE_FALSE(r.ok());
  }
}

TEST_CASE("Malformed config file line is rejected", "[config]") {
  OptionRegistry reg = MakeRegistry();
  const std::string path =
      WriteTemp("malformed", "net.http_port 7000\n");  // missing '='
  std::vector<const char*> argv = {"codicis"};
  Result<Config> r = Config::load_with_file(
      reg, path, static_cast<int>(argv.size()), argv.data());
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.error().code == ErrorCode::kInvalidArg);
}
