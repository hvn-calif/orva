#include "fuzz/suppress.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

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

std::string Trim(const std::string& text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

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

DivergenceId LookupDivergence(const std::string& wire_name) {
  for (size_t i = 0; i < kRegistrySize; ++i) {
    if (wire_name == kRegistry[i].wire) return kRegistry[i].id;
  }
  return DivergenceId::kNone;
}

void SuppressionSet::Add(DivergenceId id, const std::string& evidence) {
  auto& evidences = entries_[id];
  if (!evidence.empty()) evidences.push_back(evidence);
}

bool SuppressionSet::Contains(DivergenceId id,
                              const std::string& evidence) const {
  auto it = entries_.find(id);
  if (it == entries_.end()) return false;
  // No evidence strings means suppress this ID unconditionally.
  if (it->second.empty()) return true;
  for (const std::string& needle : it->second) {
    if (evidence.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string SuppressionSet::Render() const {
  if (entries_.empty()) return "(none)";
  std::string out;
  // entries_ is a std::map, so this is ordered and therefore reproducible.
  for (const auto& [id, evidences] : entries_) {
    if (!out.empty()) out += ",";
    out += DivergenceName(id);
    for (const std::string& evidence : evidences) {
      out += ":" + evidence;
    }
  }
  return out;
}

bool ParseSuppressions(const std::string& text, SuppressionSet* out,
                       std::string* error) {
  std::istringstream stream(text);
  std::string line;
  int line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    line = Trim(line);
    if (line.empty()) continue;

    std::string name = line;
    std::string evidence;
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      name = Trim(line.substr(0, colon));
      evidence = Trim(line.substr(colon + 1));
    }

    DivergenceId id = LookupDivergence(name);
    if (id == DivergenceId::kNone) {
      // Fatal rather than ignored: a typo that silently suppresses nothing
      // would leave the operator believing a divergence was muted.
      if (error != nullptr) {
        *error = "line " + std::to_string(line_number) +
                 ": unknown divergence ID '" + name + "'";
      }
      return false;
    }
    out->Add(id, evidence);
  }
  return true;
}

std::string Finding::Render() const {
  std::string out = "[";
  out += DivergenceName(id);
  out += "] ";
  out += DivergenceDoc(id);
  out += "\n    at: " + (path.empty() ? std::string("$") : path);
  if (!detail.empty()) out += "\n    " + detail;
  return out;
}

bool FindingLog::Report(DivergenceId id, const std::string& path,
                        const std::string& detail,
                        const std::string& evidence) {
  const std::string& match_against = evidence.empty() ? detail : evidence;
  if (suppressions_ != nullptr && suppressions_->Contains(id, match_against)) {
    ++suppressed_count_;
    return false;
  }
  findings_.push_back(Finding{id, path, detail, evidence});
  return true;
}

std::string FindingLog::Render() const {
  std::string out;
  for (const Finding& finding : findings_) {
    out += finding.Render();
    out += "\n";
  }
  if (suppressed_count_ != 0) {
    out += "  (" + std::to_string(suppressed_count_) +
           " further difference(s) suppressed)\n";
  }
  // A reproducer captured under one suppression set is uninterpretable under
  // another, so the set is part of every failure message.
  out += "  suppressions in effect: " +
         (suppressions_ != nullptr ? suppressions_->Render() : "(none)") + "\n";
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
        std::fprintf(stderr, "avro fuzz: %s: %s\n", file_path.c_str(),
                     error.c_str());
        std::abort();
      }
    } else if (path != nullptr) {
      // An explicitly named file that does not exist is a configuration
      // error; a missing default file just means "suppress nothing".
      std::fprintf(stderr, "avro fuzz: cannot open %s\n", file_path.c_str());
      std::abort();
    }

    const char* inline_list = std::getenv("AVRO_FUZZ_SUPPRESS");
    if (inline_list != nullptr) {
      std::string body(inline_list);
      for (char& c : body) {
        if (c == ',') c = '\n';
      }
      if (!ParseSuppressions(body, set, &error)) {
        std::fprintf(stderr, "avro fuzz: AVRO_FUZZ_SUPPRESS: %s\n",
                     error.c_str());
        std::abort();
      }
    }
    return set;
  }();
  return *resolved;
}

}  // namespace security::avro_fuzz
