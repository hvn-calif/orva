#include "fuzz/suppress.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"

namespace security::avro_fuzz {
namespace {

struct Entry {
  DivergenceId id;
  const char* wire;
  const char* doc;
};

constexpr Entry kRegistry[] = {
#define AVRO_FUZZ_ENTRY(name, wire, doc) {DivergenceId::name, wire, doc},
    AVRO_FUZZ_DIVERGENCES(AVRO_FUZZ_ENTRY)
#undef AVRO_FUZZ_ENTRY
};

constexpr size_t kRegistrySize = sizeof(kRegistry) / sizeof(kRegistry[0]);

const char* kDefaultSuppressionFile = "fuzz/suppressions.txt";

}  // namespace

const char* DivergenceName(DivergenceId id) {
  for (size_t i = 0; i < kRegistrySize; ++i) {
    if (kRegistry[i].id == id) return kRegistry[i].wire;
  }
  return "NONE";
}

const char* DivergenceDoc(DivergenceId id) {
  for (size_t i = 0; i < kRegistrySize; ++i) {
    if (kRegistry[i].id == id) return kRegistry[i].doc;
  }
  return "";
}

DivergenceId LookupDivergence(absl::string_view wire_name) {
  for (size_t i = 0; i < kRegistrySize; ++i) {
    if (wire_name == kRegistry[i].wire) return kRegistry[i].id;
  }
  return DivergenceId::kNone;
}

void SuppressionSet::Add(DivergenceId id, absl::string_view evidence) {
  auto& evidences = entries_[id];
  if (!evidence.empty()) evidences.emplace_back(evidence);
}

bool SuppressionSet::Contains(DivergenceId id,
                              absl::string_view evidence) const {
  auto it = entries_.find(id);
  if (it == entries_.end()) return false;
  // No evidence strings means suppress this ID unconditionally.
  if (it->second.empty()) return true;
  for (const std::string& needle : it->second) {
    if (absl::StrContains(evidence, needle)) return true;
  }
  return false;
}

std::string SuppressionSet::Render() const {
  if (entries_.empty()) return "(none)";
  // entries_ is a std::map, so this is ordered and therefore reproducible.
  return absl::StrJoin(
      entries_, ",",
      [](std::string* out,
         const std::pair<const DivergenceId, std::vector<std::string>>& entry) {
        absl::StrAppend(out, DivergenceName(entry.first));
        for (const std::string& evidence : entry.second) {
          absl::StrAppend(out, ":", evidence);
        }
      });
}

bool ParseSuppressions(absl::string_view text, SuppressionSet* out,
                       std::string* error) {
  int line_number = 0;
  for (absl::string_view raw_line : absl::StrSplit(text, '\n')) {
    ++line_number;
    absl::string_view line =
        absl::StripAsciiWhitespace(raw_line.substr(0, raw_line.find('#')));
    if (line.empty()) continue;

    absl::string_view name = line;
    absl::string_view evidence;
    const size_t colon = line.find(':');
    if (colon != absl::string_view::npos) {
      name = absl::StripAsciiWhitespace(line.substr(0, colon));
      evidence = absl::StripAsciiWhitespace(line.substr(colon + 1));
    }

    const DivergenceId id = LookupDivergence(name);
    if (id == DivergenceId::kNone) {
      // Fatal rather than ignored: a typo that silently suppresses nothing
      // would leave the operator believing a divergence was muted.
      if (error != nullptr) {
        *error = absl::StrCat("line ", line_number, ": unknown divergence ID '",
                              name, "'");
      }
      return false;
    }
    out->Add(id, evidence);
  }
  return true;
}

std::string Finding::Render() const {
  std::string out = absl::StrCat(
      "[", DivergenceName(id), "] ", DivergenceDoc(id), "\n    at: ",
      path.empty() ? absl::string_view("$") : absl::string_view(path));
  if (!detail.empty()) absl::StrAppend(&out, "\n    ", detail);
  return out;
}

bool FindingLog::Report(DivergenceId id, absl::string_view path,
                        absl::string_view detail, absl::string_view evidence) {
  const absl::string_view match_against = evidence.empty() ? detail : evidence;
  if (suppressions_ != nullptr && suppressions_->Contains(id, match_against)) {
    ++suppressed_count_;
    return false;
  }
  findings_.push_back(Finding{id, std::string(path), std::string(detail),
                              std::string(evidence)});
  return true;
}

std::string FindingLog::Render() const {
  std::string out;
  for (const Finding& finding : findings_) {
    absl::StrAppend(&out, finding.Render(), "\n");
  }
  if (suppressed_count_ != 0) {
    absl::StrAppend(&out, "  (", suppressed_count_,
                    " further difference(s) suppressed)\n");
  }
  // A reproducer captured under one suppression set is uninterpretable under
  // another, so the set is part of every failure message.
  absl::StrAppend(
      &out, "  suppressions in effect: ",
      suppressions_ != nullptr ? suppressions_->Render() : "(none)", "\n");
  return out;
}

const SuppressionSet& Suppressions() {
  static const SuppressionSet* resolved = [] {
    auto* set = new SuppressionSet();
    std::string error;

    const char* path = std::getenv("AVRO_FUZZ_SUPPRESS_FILE");
    std::string file_path = path != nullptr ? path : kDefaultSuppressionFile;
    std::ifstream file(file_path);
    if (file) {
      std::stringstream body;
      body << file.rdbuf();
      if (!ParseSuppressions(body.str(), set, &error)) {
        absl::FPrintF(stderr, "avro fuzz: %s: %s\n", file_path, error);
        std::abort();
      }
    } else if (path != nullptr) {
      // An explicitly named file that does not exist is a configuration
      // error; a missing default file just means "suppress nothing".
      absl::FPrintF(stderr, "avro fuzz: cannot open %s\n", file_path);
      std::abort();
    }

    const char* inline_list = std::getenv("AVRO_FUZZ_SUPPRESS");
    if (inline_list != nullptr) {
      const std::string body = absl::StrReplaceAll(inline_list, {{",", "\n"}});
      if (!ParseSuppressions(body, set, &error)) {
        absl::FPrintF(stderr, "avro fuzz: AVRO_FUZZ_SUPPRESS: %s\n", error);
        std::abort();
      }
    }
    return set;
  }();
  return *resolved;
}

}  // namespace security::avro_fuzz
